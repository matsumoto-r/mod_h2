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

#ifndef __mod_h2__h2_config_h__
#define __mod_h2__h2_config_h__

#undef PACKAGE_VERSION
#undef PACKAGE_TARNAME
#undef PACKAGE_STRING
#undef PACKAGE_NAME
#undef PACKAGE_BUGREPORT
#include "config.h"

typedef enum {
    H2_CONF_ENABLED,
    H2_CONF_MAX_STREAMS,
    H2_CONF_MAX_HL_SIZE,
    H2_CONF_WIN_SIZE,
    H2_CONF_MIN_WORKERS,
    H2_CONF_MAX_WORKERS,
    H2_CONF_STREAM_MAX_MEM_SIZE,
} h2_config_var_t;

/* Apache httpd module configuration for h2. */
typedef struct h2_config {
    const char *name;
    int h2_enabled;             /* if mod_h2 is active at all here */
    int h2_max_streams;         /* max concurrent # streams (http2) */
    int h2_max_hl_size;         /* max header list size (http2) */
    int h2_window_size;         /* stream window size (http2) */
    int min_workers;            /* min # of worker threads/child */
    int max_workers;            /* max # of worker threads/child */
    int stream_max_mem_size;    /* max # bytes held in memory/stream */
} h2_config;


void *h2_config_create_dir(apr_pool_t *pool, char *x);
void *h2_config_create_svr(apr_pool_t *pool, server_rec *s);
void *h2_config_merge(apr_pool_t *pool, void *basev, void *addv);

apr_status_t h2_config_apply_header(h2_config *config, request_rec *r);

extern const command_rec h2_cmds[];

h2_config *h2_config_get(conn_rec *c);
h2_config *h2_config_sget(server_rec *s);
h2_config *h2_config_rget(request_rec *r);

int h2_config_geti(h2_config *conf, h2_config_var_t var);

#endif /* __mod_h2__h2_config_h__ */

