/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stdio.h>

#include <apr_strings.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>
#include <http_connection.h>

#include "h2_private.h"
#include "h2_bucket.h"
#include "h2_bucket_queue.h"
#include "h2_mplx.h"
#include "h2_response.h"
#include "h2_to_h1.h"
#include "h2_util.h"

#define HTTP_RLINE_SUFFIX       " HTTP/1.1\r\n"
#define HTTP_RLINE_SUFFIX_LEN   11


struct h2_to_h1 {
    h2_bucket *data;
    int stream_id;
    int eoh;
    int eos;
    int flushed;
};

h2_to_h1 *h2_to_h1_create(apr_pool_t *pool)
{
    h2_to_h1 *to_h1 = apr_pcalloc(pool, sizeof(h2_to_h1));
    if (to_h1) {
        // nop
    }
    return to_h1;
}

void h2_to_h1_destroy(h2_to_h1 *to_h1)
{
    if (to_h1->data) {
        h2_bucket_destroy(to_h1->data);
        to_h1->data = NULL;
    }
}

static apr_status_t ensure_data(h2_to_h1 *to_h1, apr_size_t size)
{
    if (!to_h1->data) {
        to_h1->data = h2_bucket_alloc(size);
        if (!to_h1->data) {
            return APR_ENOMEM;
        }
    }
    return APR_SUCCESS;
}


apr_status_t h2_to_h1_start_request(h2_to_h1 *to_h1,
                                    int stream_id,
                                    const char *method,
                                    const char *path,
                                    const char *authority,
                                    h2_mplx *m)
{
    apr_status_t status = APR_SUCCESS;
    if (!method) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, h2_mplx_get_pool(m),
                      "h2_to_h1: header start but :method missing");
        return APR_EGENERAL;
    }
    if (!path) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, 0, h2_mplx_get_pool(m),
                      "h2_to_h1: header start but :path missing");
        return APR_EGENERAL;
    }
    
    status = ensure_data(to_h1, BLOCKSIZE);
    if (status != APR_SUCCESS) {
        return status;
    }
    
    size_t mlen = strlen(method);
    size_t plen = strlen(path);
    size_t total = mlen + 1 + plen + HTTP_RLINE_SUFFIX_LEN;
    if (!h2_bucket_has_free(to_h1->data, total)) {
        ap_log_perror(APLOG_MARK, APLOG_ERR, APR_ENAMETOOLONG,
                      h2_mplx_get_pool(m), "h2_to_h1: adding request line");
        return APR_ENAMETOOLONG;
    }
    h2_bucket_append(to_h1->data, method, mlen);
    h2_bucket_append(to_h1->data, " ", 1);
    h2_bucket_append(to_h1->data, path, plen);
    h2_bucket_append(to_h1->data, HTTP_RLINE_SUFFIX, HTTP_RLINE_SUFFIX_LEN);

    if (authority) {
        status = h2_to_h1_add_header(to_h1, "Host", 4,
                                     authority, strlen(authority), m);
    }
    return status;
}


static apr_status_t append_header(h2_bucket *bucket,
                                  const char *name, size_t nlen,
                                  const char *value, size_t vlen)
{
    if (nlen > 0) {
        size_t total = nlen + vlen + 4;
        if (!h2_bucket_has_free(bucket, total)) {
            return APR_ENAMETOOLONG;
        }
        h2_bucket_append(bucket, name, nlen);
        h2_bucket_append(bucket, ": ", 2);
        if (vlen > 0) {
            h2_bucket_append(bucket, value, vlen);
        }
        h2_bucket_append(bucket, "\r\n", 2);
    }
    return APR_SUCCESS;
}

apr_status_t h2_to_h1_flush(h2_to_h1 *to_h1, struct h2_mplx *m)
{
    apr_status_t status = APR_SUCCESS;
    int write_close = !to_h1->flushed && to_h1->eos;
    if (to_h1->data) {
        status = h2_mplx_in_write(m, to_h1->stream_id, to_h1->data);
        to_h1->data = NULL;
        if (status == APR_SUCCESS) {
            to_h1->flushed = 1;
        }
        else {
            ap_log_perror(APLOG_MARK, APLOG_ERR, status,
                          h2_mplx_get_pool(m),
                          "h2_request(%d): pushing request data",
                          to_h1->stream_id);
        }
    }
    
    if (write_close) {
        h2_mplx_in_close(m, to_h1->stream_id);
    }
    return status;
}


apr_status_t h2_to_h1_add_header(h2_to_h1 *to_h1,
                                 const char *name, size_t nlen,
                                 const char *value, size_t vlen,
                                 h2_mplx *m)
{
    apr_status_t status = append_header(to_h1->data, name, nlen, value, vlen);
    if (status == APR_ENAMETOOLONG && to_h1->data->data_len > 0) {
        /* header did not fit into bucket, push bucket to input and
         * get a new one */
        status = h2_to_h1_flush(to_h1, m);
        if (status == APR_SUCCESS) {
            status = append_header(to_h1->data, name, nlen, value, vlen);
            /* if this still does not work, we fail */
        }
    }
    return status;
}


apr_status_t h2_to_h1_end_headers(h2_to_h1 *to_h1, struct h2_mplx *m)
{
    if (to_h1->eoh) {
        return APR_EINVAL;
    }
    
    apr_status_t status = ensure_data(to_h1, BLOCKSIZE);
    if (status != APR_SUCCESS) {
        return status;
    }
    
    if (!h2_bucket_has_free(to_h1->data, 2)) {
        status = h2_to_h1_flush(to_h1, m);
    }
    
    if (status == APR_SUCCESS) {
        h2_bucket_cat(to_h1->data, "\r\n");
    }
    to_h1->eoh = 1;
    return status;
}

apr_status_t h2_to_h1_add_data(h2_to_h1 *to_h1,
                               const char *data, size_t len,
                               struct h2_mplx *m)
{
    if (to_h1->eos || !to_h1->eoh) {
        return APR_EINVAL;
    }
    
    // TODO: if input may have a body and with have not seen any
    // content-length header, we need to chunk the input data!!!
    //
    apr_status_t status = ensure_data(to_h1, DATA_BLOCKSIZE);
    if (status != APR_SUCCESS) {
        return status;
    }
    
    while (len > 0) {
        apr_size_t written = h2_bucket_append(to_h1->data, data, len);
        if (written < len) {
            len -= written;
            data += written;
            apr_status_t status = h2_to_h1_flush(to_h1, m);
            if (status != APR_SUCCESS) {
                return status;
            }
        }
        else {
            len = 0;
        }
    }
    return APR_SUCCESS;

}

apr_status_t h2_to_h1_close(h2_to_h1 *to_h1, struct h2_mplx *m)
{
    apr_status_t status = APR_SUCCESS;
    if (!to_h1->eos) {
        to_h1->eos = 1;
        if (to_h1->flushed) {
            status = h2_to_h1_flush(to_h1, m);
            if (status == APR_SUCCESS) {
                status = h2_mplx_in_close(m, to_h1->stream_id);
            }
        }
    }
    return status;
}

h2_bucket *h2_to_h1_steal_first_data(h2_to_h1 *to_h1, int *peos)
{
    *peos = 0;
    if (!to_h1->flushed && to_h1->data) {
        h2_bucket *data = to_h1->data;
        to_h1->data = NULL;
        to_h1->flushed = 1;
        *peos = to_h1->eos;
        return data;
    }
    return NULL;
}


