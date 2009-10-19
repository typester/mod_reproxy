#include <sys/types.h>

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>

#include "buffer.h"
#include "server.h"
#include "keyvalue.h"
#include "log.h"

#include "http_chunk.h"
#include "fdevent.h"
#include "connections.h"
#include "response.h"
#include "joblist.h"
#include "version.h"

#include "plugin.h"

#ifdef HAVE_SYS_FILIO_H
# include <sys/filio.h>
#endif

#include "sys-socket.h"

#define data_proxy data_fastcgi
#define data_proxy_init data_fastcgi_init

/* plugin config for all request/connections */
typedef struct {
    unsigned short debug;
    unsigned short enabled;
} plugin_config;

typedef struct {
    PLUGIN_DATA;
    plugin_config **config_storage;
    plugin_config conf;

    void **original_handler;

    buffer *parse_response;
} plugin_data;

typedef enum {
    PROXY_STATE_INIT,
    PROXY_STATE_CONNECT,
    PROXY_STATE_CONNECT_DELAY,
    PROXY_STATE_PREPARE_WRITE,
    PROXY_STATE_WRITE,
    PROXY_STATE_READ,
    PROXY_STATE_ERROR
} proxy_connection_state_t;

typedef struct {
    proxy_connection_state_t state;
    time_t state_timestamp;

    buffer *host;
    int port;
    buffer *path;

    buffer *response;
    buffer *response_header;

    chunkqueue *wb;

    int fd;                     /* fd to the proxy process */
    int fde_ndx;                /* index into the fd-event buffer */

    connection *remote_conn;    /* dump pointer */
    plugin_data *plugin_data;   /* dump pointer */
} handler_ctx;

static handler_ctx * handler_ctx_init() {
    handler_ctx *hctx;
    hctx = calloc(1, sizeof(handler_ctx));

    hctx->state = PROXY_STATE_INIT;

    hctx->host = buffer_init();
    hctx->path = buffer_init();

    hctx->response = buffer_init();
    hctx->response_header = buffer_init();
    hctx->wb = chunkqueue_init();

    hctx->fd = -1;
    hctx->fde_ndx = -1;

    return hctx;
}

static void handler_ctx_free(handler_ctx *hctx) {
    buffer_free(hctx->host);
    buffer_free(hctx->path);

    buffer_free(hctx->response);
    buffer_free(hctx->response_header);
    chunkqueue_free(hctx->wb);

    free(hctx);
}

/* prototypes */
typedef handler_t (*plugin_handler_subrequest)(server *srv, connection *con, void *p_d);
static handler_t mod_reproxy_proxy_handler(server *srv, handler_ctx *hctx);
SUBREQUEST_FUNC(mod_reproxy_subrequest);

/* init the plugin data */
INIT_FUNC(mod_reproxy_init) {
    plugin_data *p;
    p = calloc(1, sizeof(*p));
    p->parse_response = buffer_init();

    return p;
}

/* detroy the plugin data */
FREE_FUNC(mod_reproxy_free) {
    plugin_data *p = p_d;

    if (!p) return HANDLER_GO_ON;

    if (p->config_storage) {
        size_t i;

        for (i = 0; i < srv->config_context->used; i++) {
            plugin_config *s = p->config_storage[i];

            if (!s) continue;
            free(s);
        }
        free(p->config_storage);
    }

    if (p->original_handler) {
        free(p->original_handler);
    }

    buffer_free(p->parse_response);

    free(p);

    return HANDLER_GO_ON;
}

/* handle plugin config and check values */
SETDEFAULTS_FUNC(mod_reproxy_set_defaults) {
    plugin_data *p = p_d;
    size_t i = 0;

    config_values_t cv[] = {
        { "reproxy.enable", NULL, T_CONFIG_BOOLEAN, T_CONFIG_SCOPE_CONNECTION }, /* 0 */
        { "reproxy.debug",  NULL, T_CONFIG_BOOLEAN, T_CONFIG_SCOPE_CONNECTION }, /* 1 */
        { NULL,             NULL, T_CONFIG_UNSET,   T_CONFIG_SCOPE_UNSET }
    };

    if (!p) return HANDLER_ERROR;

    p->config_storage = calloc(1, srv->config_context->used * sizeof(specific_config *));

    for (i = 0; i < srv->config_context->used; i++) {
        plugin_config *s;

        s = calloc(1, sizeof(plugin_config));

        cv[0].destination = &(s->enabled);
        cv[1].destination = &(s->debug);

        p->config_storage[i] = s;

        if (0 != config_insert_values_global(srv, ((data_config *)srv->config_context->data[i])->value, cv)) {
            return HANDLER_ERROR;
        }
    }

    p->original_handler = calloc(1, (srv->plugins.used + 1) * sizeof(void *));
    for (i = 0; i < srv->plugins.used; i++) {
        plugin *_p = ((plugin **)(srv->plugins.ptr))[i];
        plugin_data *_pd = _p->data;

        if (!_pd) continue;
        if (_pd->id == p->id) continue;

        if (_p->handle_subrequest) {
            p->original_handler[_pd->id] = _p->handle_subrequest;
            _p->handle_subrequest = mod_reproxy_subrequest;
        }
    }

    return HANDLER_GO_ON;
}

#define PATCH(x) \
    p->conf.x = s->x;
static int mod_reproxy_patch_connection(server *srv, connection *con, plugin_data *p) {
    size_t i, j;
    plugin_config *s = p->config_storage[0];

    PATCH(enabled);
    PATCH(debug);

    /* skip the first, the global context */
    for (i = 1; i < srv->config_context->used; i++) {
        data_config *dc = (data_config *)srv->config_context->data[i];
        s = p->config_storage[i];

        /* condition didn't match */
        if (!config_check_cond(srv, con, dc)) continue;

        /* merge config */
        for (j = 0; j < dc->value->used; j++) {
            data_unset *du = dc->value->data[j];

            if (buffer_is_equal_string(du->key, CONST_STR_LEN("reproxy.enable"))) {
                PATCH(enabled);
            }
            else if (buffer_is_equal_string(du->key, CONST_STR_LEN("reproxy.debug"))) {
                PATCH(debug);
            }
        }
    }

    return 0;
}
#undef PATCH

static int proxy_set_state(server *srv, handler_ctx *hctx, proxy_connection_state_t state) {
    hctx->state = state;
    hctx->state_timestamp = srv->cur_ts;

    return 0;
}

static void proxy_set_header(connection *con, const char *key, const char *value) {
    data_string *ds_dst;

    if (NULL == (ds_dst = (data_string *)array_get_unused_element(con->request.headers, TYPE_STRING))) {
          ds_dst = data_string_init();
    }

    buffer_copy_string(ds_dst->key, key);
    buffer_copy_string(ds_dst->value, value);
    array_insert_unique(con->request.headers, (data_unset *)ds_dst);
}

static void proxy_append_header(connection *con, const char *key, const char *value) {
    data_string *ds_dst;

    if (NULL == (ds_dst = (data_string *)array_get_unused_element(con->request.headers, TYPE_STRING))) {
          ds_dst = data_string_init();
    }

    buffer_copy_string(ds_dst->key, key);
    buffer_append_string(ds_dst->value, value);
    array_insert_unique(con->request.headers, (data_unset *)ds_dst);
}

static int proxy_create_env(server *srv, handler_ctx *hctx) {
    size_t i;

    connection *con   = hctx->remote_conn;
    buffer *b;

    /* build header */
    b = chunkqueue_get_append_buffer(hctx->wb);

    /* request line */
    buffer_append_string_len(b, CONST_STR_LEN("GET "));
    buffer_append_string_buffer(b, hctx->path);
    buffer_append_string_len(b, CONST_STR_LEN(" HTTP/1.0\r\n"));

    /* request header */
    buffer_append_string_len(b, CONST_STR_LEN("Host: "));
    buffer_append_string_buffer(b, hctx->host);
    buffer_append_string_len(b, CONST_STR_LEN("\r\n"));

    buffer_append_string_len(b, CONST_STR_LEN("Connection: clone\r\n"));
    buffer_append_string_len(b, CONST_STR_LEN("User-Agent: mod_reproxy ("));
    if (buffer_is_empty(con->conf.server_tag)) {
        buffer_append_string_len(b, CONST_STR_LEN(PACKAGE_DESC));
    } else {
        buffer_append_string_len(b, CONST_BUF_LEN(con->conf.server_tag));
    }
    buffer_append_string_len(b, CONST_STR_LEN(")\r\n"));

    buffer_append_string_len(b, CONST_STR_LEN("\r\n"));

    hctx->wb->bytes_in += b->used - 1;

    return 0;
}

static void proxy_connection_close(server *srv, handler_ctx *hctx) {
    plugin_data *p;
    connection *con;

    if (NULL == hctx) return;

    p    = hctx->plugin_data;
    con  = hctx->remote_conn;

    if (hctx->fd != -1) {
        fdevent_event_del(srv->ev, &(hctx->fde_ndx), hctx->fd);
        fdevent_unregister(srv->ev, hctx->fd);

        close(hctx->fd);
        hctx->fd = -1;

        srv->cur_fds--;
    }
}

static int proxy_response_parse(server *srv, connection *con, plugin_data *p, buffer *in) {
    char *s, *ns;
    int http_response_status = -1;

    UNUSED(srv);

    /* \r\n -> \0\0 */

    buffer_copy_string_buffer(p->parse_response, in);

    for (s = p->parse_response->ptr; NULL != (ns = strstr(s, "\r\n")); s = ns + 2) {
        char *key, *value;
        int key_len;
        data_string *ds;
        int copy_header;

        ns[0] = '\0';
        ns[1] = '\0';

        if (-1 == http_response_status) {
            /* The first line of a Response message is the Status-Line */

            for (key=s; *key && *key != ' '; key++);

            if (*key) {
                http_response_status = (int) strtol(key, NULL, 10);
                if (http_response_status <= 0) http_response_status = 502;
            } else {
                http_response_status = 502;
            }

            con->http_status = http_response_status;
            con->parsed_response |= HTTP_STATUS;
            continue;
        }

        if (NULL == (value = strchr(s, ':'))) {
            /* now we expect: "<key>: <value>\n" */

            continue;
        }

        key = s;
        key_len = value - key;

        value++;
        /* strip WS */
        while (*value == ' ' || *value == '\t') value++;

        copy_header = 1;

        switch(key_len) {
        case 4:
            if (0 == strncasecmp(key, "Date", key_len)) {
                con->parsed_response |= HTTP_DATE;
            }
            break;
        case 8:
            if (0 == strncasecmp(key, "Location", key_len)) {
                con->parsed_response |= HTTP_LOCATION;
            }
            break;
        case 10:
            if (0 == strncasecmp(key, "Connection", key_len)) {
                copy_header = 0;
            }
            break;
        case 14:
            if (0 == strncasecmp(key, "Content-Length", key_len)) {
                con->response.content_length = strtol(value, NULL, 10);
                con->parsed_response |= HTTP_CONTENT_LENGTH;
            }
            break;
        default:
            break;
        }

        if (copy_header) {
            if (NULL == (ds = (data_string *)array_get_unused_element(con->response.headers, TYPE_STRING))) {
                ds = data_response_init();
            }
            buffer_copy_string_len(ds->key, key, key_len);
            buffer_copy_string(ds->value, value);

            array_insert_unique(con->response.headers, (data_unset *)ds);
        }
    }

    return 0;
}

static int proxy_demux_response(server *srv, handler_ctx *hctx) {
    int fin = 0;
    int b;
    ssize_t r;

    plugin_data *p    = hctx->plugin_data;
    connection *con   = hctx->remote_conn;
    int proxy_fd       = hctx->fd;

    /* check how much we have to read */
    if (ioctl(hctx->fd, FIONREAD, &b)) {
        log_error_write(srv, __FILE__, __LINE__, "sd",
            "ioctl failed: ", proxy_fd);
        return -1;
    }

    if (p->conf.debug) {
        log_error_write(srv, __FILE__, __LINE__, "sd", "proxy - have to read:", b);
    }

    if (b > 0) {
        if (hctx->response->used == 0) {
            /* avoid too small buffer */
            buffer_prepare_append(hctx->response, b + 1);
            hctx->response->used = 1;
        } else {
            buffer_prepare_append(hctx->response, b);
        }

        if (-1 == (r = read(hctx->fd, hctx->response->ptr + hctx->response->used - 1, b))) {
            if (errno == EAGAIN) return 0;
            log_error_write(srv, __FILE__, __LINE__, "sds",
                "unexpected end-of-file (perhaps the proxy process died):",
                proxy_fd, strerror(errno));
            return -1;
        }

        /* this should be catched by the b > 0 above */
        assert(r);

        hctx->response->used += r;
        hctx->response->ptr[hctx->response->used - 1] = '\0';

#if 0
        log_error_write(srv, __FILE__, __LINE__, "sdsbs",
                "demux: Response buffer len", hctx->response->used, ":", hctx->response, ":");
#endif

        if (0 == con->got_response) {
            con->got_response = 1;
            buffer_prepare_copy(hctx->response_header, 128);
        }

        if (0 == con->file_started) {
            char *c;

            /* search for the \r\n\r\n in the string */
            if (NULL != (c = buffer_search_string_len(hctx->response, "\r\n\r\n", 4))) {
                size_t hlen = c - hctx->response->ptr + 4;
                size_t blen = hctx->response->used - hlen - 1;
                /* found */

                buffer_append_string_len(hctx->response_header, hctx->response->ptr, c - hctx->response->ptr + 4);
#if 0
                log_error_write(srv, __FILE__, __LINE__, "sb", "Header:", hctx->response_header);
#endif
                /* parse the response header */
                proxy_response_parse(srv, con, p, hctx->response_header);

                /* enable chunked-transfer-encoding */
                if (con->request.http_version == HTTP_VERSION_1_1 &&
                    !(con->parsed_response & HTTP_CONTENT_LENGTH)) {
                    con->response.transfer_encoding = HTTP_TRANSFER_ENCODING_CHUNKED;
                }
                else {
                    con->response.transfer_encoding = HTTP_TRANSFER_ENCODING_IDENTITY;
                }

                con->file_started = 1;
                if (blen) {
                    http_chunk_append_mem(srv, con, c + 4, blen + 1);
                    joblist_append(srv, con);
                }
                hctx->response->used = 0;
            }
        } else {
            http_chunk_append_mem(srv, con, hctx->response->ptr, hctx->response->used);
            joblist_append(srv, con);
            hctx->response->used = 0;
        }

    } else {
        /* reading from upstream done */
        con->file_finished = 1;

        http_chunk_append_mem(srv, con, NULL, 0);
        joblist_append(srv, con);

        fin = 1;
    }

    return fin;
}

static handler_t proxy_handle_fdevent(void *s, void *ctx, int revents) {
    server      *srv  = (server *)s;
    handler_ctx *hctx = ctx;
    connection  *con  = hctx->remote_conn;
    plugin_data *p    = hctx->plugin_data;

    if ((revents & FDEVENT_IN) &&
        hctx->state == PROXY_STATE_READ) {

        if (p->conf.debug) {
            log_error_write(srv, __FILE__, __LINE__, "sd",
                "proxy: fdevent-in", hctx->state);
        }

        switch (proxy_demux_response(srv, hctx)) {
            case 0:
                break;
            case 1:
                /* we are done */
                proxy_connection_close(srv, hctx);

                joblist_append(srv, con);
                return HANDLER_FINISHED;
            case -1:
                if (con->file_started == 0) {
                    /* nothing has been send out yet, send a 500 */
                    connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST);
                    con->http_status = 500;
                    //con->mode = DIRECT;
                } else {
                    /* response might have been already started, kill the connection */
                    connection_set_state(srv, con, CON_STATE_ERROR);
                }

                joblist_append(srv, con);
                return HANDLER_FINISHED;
        }
    }

    if (revents & FDEVENT_OUT) {
        if (p->conf.debug) {
            log_error_write(srv, __FILE__, __LINE__, "sd",
                "proxy: fdevent-out", hctx->state);
        }

        if (hctx->state == PROXY_STATE_CONNECT ||
            hctx->state == PROXY_STATE_WRITE) {
            /* we are allowed to send something out
             *
             * 1. in a unfinished connect() call
             * 2. in a unfinished write() call (long POST request)
             */
            return mod_reproxy_proxy_handler(srv, hctx);
        } else {
            log_error_write(srv, __FILE__, __LINE__, "sd",
                "proxy: out", hctx->state);
        }
    }

    /* perhaps this issue is already handled */
    if (revents & FDEVENT_HUP) {
        if (p->conf.debug) {
            log_error_write(srv, __FILE__, __LINE__, "sd",
                "proxy: fdevent-hup", hctx->state);
        }

        if (hctx->state == PROXY_STATE_CONNECT) {
            /* connect() -> EINPROGRESS -> HUP */

            /**
             * what is proxy is doing if it can't reach the next hop ?
             *
             */

            proxy_connection_close(srv, hctx);
            joblist_append(srv, con);

            con->http_status = 503;
            //con->mode = DIRECT;

            return HANDLER_FINISHED;
        }

        con->file_finished = 1;

        proxy_connection_close(srv, hctx);
        joblist_append(srv, con);
    } else if (revents & FDEVENT_ERR) {
        /* kill all connections to the proxy process */

        log_error_write(srv, __FILE__, __LINE__, "sd", "proxy-FDEVENT_ERR, but no HUP", revents);

        joblist_append(srv, con);
        proxy_connection_close(srv, hctx);
    }

    return HANDLER_FINISHED;
}

static int proxy_establish_connection(server *srv, handler_ctx *hctx) {
    struct sockaddr *proxy_addr;
    struct sockaddr_in proxy_addr_in;
    socklen_t servlen;

    plugin_data *p = hctx->plugin_data;
    int proxy_fd   = hctx->fd;

    memset(&proxy_addr_in, 0, sizeof(proxy_addr_in));
    proxy_addr_in.sin_family = AF_INET;
    proxy_addr_in.sin_addr.s_addr = inet_addr(hctx->host->ptr);
    proxy_addr_in.sin_port = htons(hctx->port);
    servlen = sizeof(proxy_addr_in);
    proxy_addr = (struct sockaddr *) &proxy_addr_in;

    if (INADDR_NONE == proxy_addr_in.sin_addr.s_addr) {
        struct hostent *h;
        h = gethostbyname(hctx->host->ptr);
        if (NULL == h) {
            log_error_write(srv, __FILE__, __LINE__, "sd",
                "gethostbyname failed: ", proxy_fd);
            return -1;
        }
        proxy_addr_in.sin_addr.s_addr = *(unsigned int *)h->h_addr_list[0];
    }

    if (-1 == connect(proxy_fd, proxy_addr, servlen)) {
        if (errno == EINPROGRESS || errno == EALREADY) {
            if (p->conf.debug) {
                log_error_write(srv, __FILE__, __LINE__, "sd",
                        "connect delayed:", proxy_fd);
            }

            return 1;
        } else {

            log_error_write(srv, __FILE__, __LINE__, "sdsd",
                    "connect failed:", proxy_fd, strerror(errno), errno);

            return -1;
        }
    }
    if (p->conf.debug) {
        log_error_write(srv, __FILE__, __LINE__, "sd",
                "connect succeeded: ", proxy_fd);
    }

    return 0;
}

static handler_t proxy_write_request(server *srv, handler_ctx *hctx) {
    plugin_data *p  = hctx->plugin_data;
    connection *con = hctx->remote_conn;

    int ret;

    switch(hctx->state) {
        case PROXY_STATE_INIT:
            if (-1 == (hctx->fd = socket(AF_INET, SOCK_STREAM, 0))) {
                log_error_write(srv, __FILE__, __LINE__, "ss",
                    "socket failed: ", strerror(errno));
                return HANDLER_ERROR;
            }
            hctx->fde_ndx = -1;
            srv->cur_fds++;

            fdevent_register(srv->ev, hctx->fd, proxy_handle_fdevent, hctx);

            if (-1 == fdevent_fcntl_set(srv->ev, hctx->fd)) {
                log_error_write(srv, __FILE__, __LINE__, "ss",
                    "fcntl failed: ", strerror(errno));

                return HANDLER_ERROR;
            }

            /* fall through */

        case PROXY_STATE_CONNECT:
            /* try to finish the connect() */
            if (hctx->state == PROXY_STATE_INIT) {
                /* first round */
                switch (proxy_establish_connection(srv, hctx)) {
                    case 1:
                        proxy_set_state(srv, hctx, PROXY_STATE_CONNECT);

                        /* connection is in progress,
                           wait for an event and call getsockopt() below */
                        fdevent_event_add(srv->ev, &(hctx->fde_ndx),
                                          hctx->fd, FDEVENT_OUT);

                        return HANDLER_WAIT_FOR_EVENT;
                    case -1:
                        /* if ECONNREFUSED choose another connection -> FIXME */
                        hctx->fde_ndx = -1;

                        return HANDLER_ERROR;
                    default:
                        /* everything is ok, go on */
                        break;
                }
            } else {
                int socket_error;
                socklen_t socket_error_len = sizeof(socket_error);

                /* we don't need it anymore */
                fdevent_event_del(srv->ev, &(hctx->fde_ndx), hctx->fd);

                /* try to finish the connect() */
                if (0 != getsockopt(hctx->fd, SOL_SOCKET, SO_ERROR,
                        &socket_error, &socket_error_len)) {
                    log_error_write(srv, __FILE__, __LINE__, "ss",
                        "getsockopt failed:", strerror(errno));

                    return HANDLER_ERROR;
                }
                if (socket_error != 0) {
                    log_error_write(srv, __FILE__, __LINE__, "ss",
                        "establishing connection failed:", strerror(socket_error),
                        "port:", hctx->port);

                    return HANDLER_ERROR;
                }
                if (p->conf.debug) {
                    log_error_write(srv, __FILE__, __LINE__,  "s",
                        "proxy - connect - delayed success");
                }
            }

            proxy_set_state(srv, hctx, PROXY_STATE_PREPARE_WRITE);
            /* fall through */
        case PROXY_STATE_PREPARE_WRITE:
            proxy_create_env(srv, hctx);

            proxy_set_state(srv, hctx, PROXY_STATE_WRITE);

            /* fall through */
        case PROXY_STATE_WRITE:;
            ret = srv->network_backend_write(srv, con, hctx->fd, hctx->wb);

            chunkqueue_remove_finished_chunks(hctx->wb);

            if (-1 == ret) {
                if (errno != EAGAIN &&
                    errno != EINTR && errno != ENOTCONN) {
                    log_error_write(srv, __FILE__, __LINE__, "ssd",
                        "write failed:", strerror(errno), errno);

                    return HANDLER_ERROR;
                } else {
                    fdevent_event_add(srv->ev, &(hctx->fde_ndx), hctx->fd, FDEVENT_OUT);

                    return HANDLER_WAIT_FOR_EVENT;
                }
            }

            if (hctx->wb->bytes_out == hctx->wb->bytes_in) {
                proxy_set_state(srv, hctx, PROXY_STATE_READ);

                fdevent_event_del(srv->ev, &(hctx->fde_ndx), hctx->fd);
                fdevent_event_add(srv->ev, &(hctx->fde_ndx), hctx->fd, FDEVENT_IN);
            } else {
                fdevent_event_add(srv->ev, &(hctx->fde_ndx), hctx->fd, FDEVENT_OUT);

                return HANDLER_WAIT_FOR_EVENT;
            }

            return HANDLER_WAIT_FOR_EVENT;
        case PROXY_STATE_READ:
            /* waiting for a response */
            return HANDLER_WAIT_FOR_EVENT;
        default:
            log_error_write(srv, __FILE__, __LINE__, "s", "(debug) unknown state");
            return HANDLER_ERROR;
    }

    return HANDLER_GO_ON;
}

static handler_t mod_reproxy_proxy_handler(server *srv, handler_ctx *hctx) {
    if (hctx->remote_conn->file_finished == 1) {
        return HANDLER_FINISHED;
    }

    switch (proxy_write_request(srv, hctx)) {
        case HANDLER_ERROR:
            proxy_connection_close(srv, hctx);
            return HANDLER_ERROR;

        case HANDLER_WAIT_FOR_EVENT:
            return HANDLER_WAIT_FOR_EVENT;
        case HANDLER_WAIT_FOR_FD:
            return HANDLER_WAIT_FOR_FD;
        default:
            break;
    }

    return HANDLER_WAIT_FOR_EVENT;
}

static int parse_url(server *srv, handler_ctx *hctx, buffer *url) {
    size_t i = 0;
    size_t s, len;

    if (0 == strncmp(url->ptr, "http://", sizeof("http://") - 1)) {
        i += sizeof("http://") - 1;
    }
    else {
        /* unsupported protocol */
        return -1;
    }

    /* host */
    s = i;
    for (; i < url->used; i++) {
        if (':' == url->ptr[i] || '/' == url->ptr[i]) {
            if ('/' == url->ptr[i]) {
                hctx->port = 80;
            }

            len = i - s;
            if (len > 0) {
                buffer_append_string_len(hctx->host, &(url->ptr[s]), len);
            }
            break;
        }
    }

    /* port */
    if (!hctx->port) {
        s = i;
        for (; i < url->used; i++) {
            if ('/' == url->ptr[i]) {
                len = i - s;
                char tmp[len + 1];
                tmp[len + 1] = "\0";
                memcpy(tmp, url->ptr[s], len);

                hctx->port = strtol(tmp, NULL, 10);
            }
            break;
        }
    }

    /* path */
    s   = i;
    len = (url->used - 1) - s;
    if (len <= 0) {
        buffer_append_string( hctx->path, "/" );
    }
    else {
        buffer_append_string_len( hctx->path, &(url->ptr[s]), len );
    }

    if (hctx->host->used > 0 && hctx->port > 0 && hctx->path->used > 0) {
        return 0;
    }
    else {
        return -1;
    }
}

SUBREQUEST_FUNC(mod_reproxy_subrequest) {
    size_t i;
    plugin_data *p;

    for (i = 0; i < srv->plugins.used; i++) {
        plugin *_p = ((plugin **)(srv->plugins.ptr))[i];
        if (buffer_is_equal_string(_p->name, CONST_STR_LEN("reproxy"))) {
            p = _p->data;
            break;
        }
    }
    if (!p) return HANDLER_ERROR;

    handler_ctx *hctx = con->plugin_ctx[p->id];
    if (hctx) {
        /* my job */
        handler_t r = mod_reproxy_proxy_handler(srv, hctx);
        return r;
    }

    plugin_handler_subrequest handler = p->original_handler[ con->mode ];
    if (handler) {
        handler_t r = handler(srv, con, p_d);

        mod_reproxy_patch_connection(srv, con, p);
        if (p->conf.enabled) {
            switch (r) {
                case HANDLER_GO_ON:
                case HANDLER_FINISHED: {
                    data_string *reproxy = (data_string *)array_get_element(
                            con->response.headers, "X-Reproxy-URL");

                    if (reproxy) {
                        buffer *reproxy_url = buffer_init_buffer(reproxy->value);

                        if (con->file_started == 1 && con->file_finished == 0) {
                            if (con->mode != DIRECT) {
                                plugin_data *d = p_d;

                                for (i = 0; i < srv->plugins.used; i++) {
                                    plugin *_p = ((plugin **)(srv->plugins.ptr))[i];
                                    plugin_data *_p_data = _p->data;
                                    if (_p_data && _p_data->id == d->id) {
                                        if (_p->connection_reset) {
                                            _p->connection_reset(srv, con, p_d);
                                        }
                                    }
                                }
                            }
                        }

                        /* ignore body */
                        chunkqueue_reset(con->write_queue);

                        hctx = handler_ctx_init();
                        if (0 != parse_url(srv, hctx, reproxy_url)) {
                            buffer_free(reproxy_url);

                            log_error_write(srv, __FILE__, __LINE__,  "sb",
                                "invalid url:", reproxy_url);

                            con->http_status = 500;
                            con->mode = DIRECT;
                            connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST);

                            return r;
                        }
                        buffer_free(reproxy_url);

                        hctx->remote_conn = con;
                        hctx->plugin_data = p;

                        con->plugin_ctx[ p->id ] = hctx;

                        array_reset(con->response.headers);
                        connection_set_state(srv, con, CON_STATE_HANDLE_REQUEST);

                        con->parsed_response = 0;
                        con->got_response = 0;
                        con->file_started = 0;
                        con->file_finished = 0;
                        array_reset(con->response.headers);

                        if (p->conf.debug) {
                            log_error_write(srv, __FILE__, __LINE__,  "s",
                                "start reproxy for:");
                            log_error_write(srv, __FILE__, __LINE__,  "sb",
                                "  host:", hctx->host);
                            log_error_write(srv, __FILE__, __LINE__,  "sd",
                                "  port:", hctx->port);
                            log_error_write(srv, __FILE__, __LINE__,  "sb",
                                "  path:", hctx->path);
                        }

                        handler_t r = mod_reproxy_proxy_handler(srv, hctx);
                        return r;
                    }
                }
                break;
            }
        }

        return r;
    }
    else {
        log_error_write(srv, __FILE__, __LINE__,  "s", "original handler not found");
        return HANDLER_GO_ON;
    }
}

static handler_t mod_reproxy_connection_close_callback(server *srv, connection *con, void *p_d) {
    plugin_data *p = p_d;
    handler_ctx *hctx = con->plugin_ctx[p->id];

    if (hctx) {
        proxy_connection_close(srv, hctx);

        handler_ctx_free(con->plugin_ctx[p->id]);
        con->plugin_ctx[p->id] = NULL;
    }

    return HANDLER_GO_ON;
}

/* this function is called at dlopen() time and inits the callbacks */
int mod_reproxy_plugin_init(plugin *p) {
    p->version = LIGHTTPD_VERSION_ID;
    p->name    = buffer_init_string("reproxy");

    p->init         = mod_reproxy_init;
    p->set_defaults = mod_reproxy_set_defaults;
    p->cleanup      = mod_reproxy_free;

    p->connection_reset = mod_reproxy_connection_close_callback;
    p->handle_connection_close = mod_reproxy_connection_close_callback;

    p->data        = NULL;

    return 0;
}
