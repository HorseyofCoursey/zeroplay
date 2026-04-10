#include "ws.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/utsname.h>

#include <libwebsockets.h>
#include <cjson/cJSON.h>

#define ZEROPLAY_VERSION "0.2.0"
#define MAX_SEND_BUF     4096
#define RECONNECT_MIN_MS 1000
#define RECONNECT_MAX_MS 30000

/* ------------------------------------------------------------------ */
/* WsCmdQueue implementation                                           */
/* ------------------------------------------------------------------ */

void ws_cmd_queue_init(WsCmdQueue *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);
}

int ws_cmd_queue_push(WsCmdQueue *q, const WsCommand *cmd)
{
    pthread_mutex_lock(&q->mutex);
    if (q->count >= WS_CMD_QUEUE_SIZE) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    q->items[q->tail] = *cmd;
    q->tail = (q->tail + 1) % WS_CMD_QUEUE_SIZE;
    q->count++;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

int ws_cmd_queue_pop(WsCmdQueue *q, WsCommand *cmd)
{
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    *cmd = q->items[q->head];
    q->head = (q->head + 1) % WS_CMD_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

void ws_cmd_queue_destroy(WsCmdQueue *q)
{
    pthread_mutex_destroy(&q->mutex);
}

/* ------------------------------------------------------------------ */
/* WsSharedState implementation                                        */
/* ------------------------------------------------------------------ */

void ws_shared_state_init(WsSharedState *s)
{
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->mutex, NULL);
    s->position_s = -1.0;
    s->idle = 1;
}

void ws_shared_state_destroy(WsSharedState *s)
{
    pthread_mutex_destroy(&s->mutex);
}

void ws_shared_state_set_position(WsSharedState *s, double pos_s)
{
    pthread_mutex_lock(&s->mutex);
    s->position_s = pos_s;
    pthread_mutex_unlock(&s->mutex);
}

void ws_shared_state_set_paused(WsSharedState *s, int paused)
{
    pthread_mutex_lock(&s->mutex);
    s->paused = paused;
    pthread_mutex_unlock(&s->mutex);
}

void ws_shared_state_set_url(WsSharedState *s, const char *url)
{
    pthread_mutex_lock(&s->mutex);
    if (url)
        snprintf(s->url, sizeof(s->url), "%s", url);
    else
        s->url[0] = '\0';
    pthread_mutex_unlock(&s->mutex);
}

void ws_shared_state_set_idle(WsSharedState *s, int idle)
{
    pthread_mutex_lock(&s->mutex);
    s->idle = idle;
    pthread_mutex_unlock(&s->mutex);
}

void ws_shared_state_set_player_ready(WsSharedState *s, int ready)
{
    pthread_mutex_lock(&s->mutex);
    s->player_ready = ready;
    pthread_mutex_unlock(&s->mutex);
}

/* ------------------------------------------------------------------ */
/* Internal WsContext definition                                       */
/* ------------------------------------------------------------------ */

struct WsContext {
    WsConfig          cfg;
    WsCmdQueue       *cmd_queue;
    WsSharedState    *shared_state;

    struct lws_context *lws_ctx;
    struct lws         *wsi;

    pthread_t          thread;
    volatile int       running;
    volatile int       connected;

    int                reconnect_delay_ms;

    /* Parsed URL components */
    char               host[256];
    int                port;
    char               path[512];
    int                use_ssl;

    /* Timers (lws sorted usec list) */
    lws_sorted_usec_list_t sul_state;
    lws_sorted_usec_list_t sul_ping;
    lws_sorted_usec_list_t sul_reconnect;

    /* Device info */
    char               hostname[64];
    char               arch[32];

    /* Pending send buffer */
    char               send_buf[LWS_PRE + MAX_SEND_BUF];
    int                send_len;
    int                send_pending;

    /* Queue of outgoing messages */
    char               out_ring[8][LWS_PRE + MAX_SEND_BUF];
    int                out_len[8];
    int                out_head;
    int                out_tail;
    int                out_count;
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int parse_ws_url(WsContext *ctx, const char *url)
{
    /* Parse ws://host:port/path or wss://host:port/path */
    ctx->use_ssl = 0;
    ctx->port = 80;

    const char *p = url;
    if (strncmp(p, "wss://", 6) == 0) {
        ctx->use_ssl = 1;
        ctx->port = 443;
        p += 6;
    } else if (strncmp(p, "ws://", 5) == 0) {
        p += 5;
    } else {
        fprintf(stderr, "ws: invalid URL scheme: %s\n", url);
        return -1;
    }

    /* Extract host:port */
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');

    if (colon && (!slash || colon < slash)) {
        int host_len = (int)(colon - p);
        if (host_len >= (int)sizeof(ctx->host)) host_len = sizeof(ctx->host) - 1;
        memcpy(ctx->host, p, (size_t)host_len);
        ctx->host[host_len] = '\0';
        ctx->port = atoi(colon + 1);
    } else if (slash) {
        int host_len = (int)(slash - p);
        if (host_len >= (int)sizeof(ctx->host)) host_len = sizeof(ctx->host) - 1;
        memcpy(ctx->host, p, (size_t)host_len);
        ctx->host[host_len] = '\0';
    } else {
        snprintf(ctx->host, sizeof(ctx->host), "%s", p);
    }

    if (slash)
        snprintf(ctx->path, sizeof(ctx->path), "%s", slash);
    else
        snprintf(ctx->path, sizeof(ctx->path), "/");

    vlog("ws: parsed url — host=%s port=%d path=%s ssl=%d\n",
         ctx->host, ctx->port, ctx->path, ctx->use_ssl);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Outgoing message ring buffer                                        */
/* ------------------------------------------------------------------ */

static int ws_queue_message(WsContext *ctx, const char *json)
{
    if (ctx->out_count >= 8) {
        fprintf(stderr, "ws: outgoing ring full, dropping message\n");
        return -1;
    }
    int idx = ctx->out_tail;
    int len = (int)strlen(json);
    if (len >= MAX_SEND_BUF) len = MAX_SEND_BUF - 1;
    memcpy(&ctx->out_ring[idx][LWS_PRE], json, (size_t)len);
    ctx->out_len[idx] = len;
    ctx->out_tail = (ctx->out_tail + 1) % 8;
    ctx->out_count++;

    if (ctx->wsi)
        lws_callback_on_writable(ctx->wsi);
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON message builders                                               */
/* ------------------------------------------------------------------ */

static void ws_send_hello(WsContext *ctx)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddStringToObject(root, "token", ctx->cfg.device_token);

    cJSON *caps = cJSON_AddObjectToObject(root, "capabilities");
    cJSON_AddStringToObject(caps, "player", "zeroplay-v4l2");
    cJSON *codecs = cJSON_AddArrayToObject(caps, "codecs");
    cJSON_AddItemToArray(codecs, cJSON_CreateString("h264"));

    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(dev, "hostname", ctx->hostname);
    cJSON_AddStringToObject(dev, "platform", "linux");
    cJSON_AddStringToObject(dev, "arch", ctx->arch);

    cJSON_AddStringToObject(root, "version", ZEROPLAY_VERSION);

    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        ws_queue_message(ctx, str);
        free(str);
    }
    cJSON_Delete(root);
}

static void ws_send_ack(WsContext *ctx, const char *op, int ok, const char *error)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddBoolToObject(root, "ok", ok);
    if (op) cJSON_AddStringToObject(root, "op", op);
    if (error) cJSON_AddStringToObject(root, "error", error);

    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        ws_queue_message(ctx, str);
        free(str);
    }
    cJSON_Delete(root);
}

static void ws_send_state(WsContext *ctx)
{
    if (!ctx->connected) return;

    WsSharedState *s = ctx->shared_state;
    double pos;
    int paused, idle;
    char url[WS_COMMAND_URL_SIZE];

    pthread_mutex_lock(&s->mutex);
    pos    = s->position_s;
    paused = s->paused;
    idle   = s->idle;
    snprintf(url, sizeof(url), "%s", s->url);
    pthread_mutex_unlock(&s->mutex);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "state");

    if (pos >= 0.0)
        cJSON_AddNumberToObject(root, "pos", pos);
    else
        cJSON_AddNullToObject(root, "pos");

    cJSON_AddBoolToObject(root, "pause", paused);

    if (url[0])
        cJSON_AddStringToObject(root, "url", url);
    else
        cJSON_AddNullToObject(root, "url");

    cJSON_AddBoolToObject(root, "idle", idle);
    cJSON_AddNumberToObject(root, "ts", (double)now_ms());

    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        ws_queue_message(ctx, str);
        free(str);
    }
    cJSON_Delete(root);
}

static void ws_send_ping(WsContext *ctx)
{
    if (!ctx->connected) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ping");
    cJSON_AddNumberToObject(root, "ts", (double)now_ms());

    char *str = cJSON_PrintUnformatted(root);
    if (str) {
        ws_queue_message(ctx, str);
        free(str);
    }
    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* Incoming message handler                                            */
/* ------------------------------------------------------------------ */

static void ws_handle_message(WsContext *ctx, const char *data, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root || !cJSON_IsObject(root)) {
        ws_send_ack(ctx, NULL, 0, "invalid_command");
        cJSON_Delete(root);
        return;
    }

    cJSON *op_item = cJSON_GetObjectItem(root, "op");
    if (!op_item || !cJSON_IsString(op_item)) {
        ws_send_ack(ctx, NULL, 0, "invalid_command");
        cJSON_Delete(root);
        return;
    }

    const char *op = op_item->valuestring;
    WsCommand cmd;
    memset(&cmd, 0, sizeof(cmd));

    if (strcmp(op, "load") == 0) {
        cJSON *url_item = cJSON_GetObjectItem(root, "url");
        if (!url_item || !cJSON_IsString(url_item) || !url_item->valuestring[0]) {
            ws_send_ack(ctx, "load", 0, "invalid_command");
            cJSON_Delete(root);
            return;
        }
        cmd.type = CMD_LOAD;
        snprintf(cmd.url, sizeof(cmd.url), "%s", url_item->valuestring);
    } else if (strcmp(op, "play") == 0) {
        cmd.type = CMD_PLAY;
    } else if (strcmp(op, "pause") == 0) {
        cmd.type = CMD_PAUSE;
    } else if (strcmp(op, "stop") == 0) {
        cmd.type = CMD_STOP;
    } else if (strcmp(op, "seek") == 0) {
        cJSON *pos_item = cJSON_GetObjectItem(root, "positionMs");
        if (!pos_item || !cJSON_IsNumber(pos_item)) {
            ws_send_ack(ctx, "seek", 0, "invalid_command");
            cJSON_Delete(root);
            return;
        }
        cmd.type = CMD_SEEK;
        cmd.seek_position_ms = (int64_t)pos_item->valuedouble;
    } else {
        ws_send_ack(ctx, NULL, 0, "invalid_command");
        cJSON_Delete(root);
        return;
    }

    if (ws_cmd_queue_push(ctx->cmd_queue, &cmd))
        ws_send_ack(ctx, op, 1, NULL);
    else
        ws_send_ack(ctx, op, 0, "command_queue_full");

    cJSON_Delete(root);
}

/* ------------------------------------------------------------------ */
/* Timer callbacks (lws_sul)                                           */
/* ------------------------------------------------------------------ */

static void sul_state_cb(lws_sorted_usec_list_t *sul)
{
    WsContext *ctx = lws_container_of(sul, WsContext, sul_state);
    ws_send_state(ctx);
    lws_sul_schedule(ctx->lws_ctx, 0, &ctx->sul_state, sul_state_cb,
                     (int64_t)ctx->cfg.state_interval_ms * 1000LL);
}

static void sul_ping_cb(lws_sorted_usec_list_t *sul)
{
    WsContext *ctx = lws_container_of(sul, WsContext, sul_ping);
    ws_send_ping(ctx);
    lws_sul_schedule(ctx->lws_ctx, 0, &ctx->sul_ping, sul_ping_cb,
                     (int64_t)ctx->cfg.ping_interval_ms * 1000LL);
}

static void sul_reconnect_cb(lws_sorted_usec_list_t *sul)
{
    WsContext *ctx = lws_container_of(sul, WsContext, sul_reconnect);

    fprintf(stderr, "ws: connecting to %s:%d%s (ssl=%d)\n",
            ctx->host, ctx->port, ctx->path, ctx->use_ssl);

    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    ccinfo.context        = ctx->lws_ctx;
    ccinfo.address        = ctx->host;
    ccinfo.port           = ctx->port;
    ccinfo.path           = ctx->path;
    ccinfo.host           = ctx->host;
    ccinfo.origin         = ctx->host;
    ccinfo.protocol       = "ws-client";
    ccinfo.userdata       = ctx;
    if (ctx->use_ssl)
        ccinfo.ssl_connection = LCCSCF_USE_SSL |
                                LCCSCF_ALLOW_SELFSIGNED |
                                LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;

    struct lws *wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        fprintf(stderr, "ws: connect failed, retrying in %dms\n",
                ctx->reconnect_delay_ms);
        lws_sul_schedule(ctx->lws_ctx, 0, &ctx->sul_reconnect,
                         sul_reconnect_cb,
                         (int64_t)ctx->reconnect_delay_ms * 1000LL);
        ctx->reconnect_delay_ms *= 2;
        if (ctx->reconnect_delay_ms > RECONNECT_MAX_MS)
            ctx->reconnect_delay_ms = RECONNECT_MAX_MS;
    }
}

/* ------------------------------------------------------------------ */
/* WebSocket client protocol callback                                  */
/* ------------------------------------------------------------------ */

static int callback_ws_client(struct lws *wsi, enum lws_callback_reasons reason,
                              void *user __attribute__((unused)),
                              void *in, size_t len)
{
    WsContext *ctx = (WsContext *)lws_context_user(lws_get_context(wsi));
    if (!ctx) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        fprintf(stderr, "ws: connected\n");
        ctx->wsi = wsi;
        ctx->connected = 1;
        ctx->reconnect_delay_ms = RECONNECT_MIN_MS;

        ws_send_hello(ctx);

        /* Start periodic timers */
        lws_sul_schedule(ctx->lws_ctx, 0, &ctx->sul_state, sul_state_cb,
                         (int64_t)ctx->cfg.state_interval_ms * 1000LL);
        lws_sul_schedule(ctx->lws_ctx, 0, &ctx->sul_ping, sul_ping_cb,
                         (int64_t)ctx->cfg.ping_interval_ms * 1000LL);
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (in && len > 0)
            ws_handle_message(ctx, (const char *)in, len);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (ctx->out_count > 0) {
            int idx = ctx->out_head;
            int n = ctx->out_len[idx];
            lws_write(wsi, (unsigned char *)&ctx->out_ring[idx][LWS_PRE],
                      (size_t)n, LWS_WRITE_TEXT);
            ctx->out_head = (ctx->out_head + 1) % 8;
            ctx->out_count--;
            if (ctx->out_count > 0)
                lws_callback_on_writable(wsi);
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "ws: connection error: %s\n",
                in ? (const char *)in : "unknown");
        ctx->wsi = NULL;
        ctx->connected = 0;
        /* Cancel timers */
        lws_sul_cancel(&ctx->sul_state);
        lws_sul_cancel(&ctx->sul_ping);
        /* Schedule reconnect */
        fprintf(stderr, "ws: reconnecting in %dms\n", ctx->reconnect_delay_ms);
        lws_sul_schedule(ctx->lws_ctx, 0, &ctx->sul_reconnect,
                         sul_reconnect_cb,
                         (int64_t)ctx->reconnect_delay_ms * 1000LL);
        ctx->reconnect_delay_ms *= 2;
        if (ctx->reconnect_delay_ms > RECONNECT_MAX_MS)
            ctx->reconnect_delay_ms = RECONNECT_MAX_MS;
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        fprintf(stderr, "ws: connection closed\n");
        ctx->wsi = NULL;
        ctx->connected = 0;
        lws_sul_cancel(&ctx->sul_state);
        lws_sul_cancel(&ctx->sul_ping);
        if (ctx->running) {
            fprintf(stderr, "ws: reconnecting in %dms\n", ctx->reconnect_delay_ms);
            lws_sul_schedule(ctx->lws_ctx, 0, &ctx->sul_reconnect,
                             sul_reconnect_cb,
                             (int64_t)ctx->reconnect_delay_ms * 1000LL);
            ctx->reconnect_delay_ms *= 2;
            if (ctx->reconnect_delay_ms > RECONNECT_MAX_MS)
                ctx->reconnect_delay_ms = RECONNECT_MAX_MS;
        }
        break;

    default:
        break;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Health HTTP callback                                                */
/* ------------------------------------------------------------------ */

static int callback_health(struct lws *wsi, enum lws_callback_reasons reason,
                           void *user __attribute__((unused)),
                           void *in __attribute__((unused)),
                           size_t len __attribute__((unused)))
{
    WsContext *ctx = (WsContext *)lws_context_user(lws_get_context(wsi));
    if (!ctx) return 0;

    switch (reason) {
    case LWS_CALLBACK_HTTP: {
        /* Build JSON response */
        int ws_conn = ctx->connected;
        int ready = 0;
        pthread_mutex_lock(&ctx->shared_state->mutex);
        ready = ctx->shared_state->player_ready;
        pthread_mutex_unlock(&ctx->shared_state->mutex);

        int ok = ws_conn && ready;
        int status = ok ? HTTP_STATUS_OK : HTTP_STATUS_SERVICE_UNAVAILABLE;

        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", ok);
        cJSON_AddBoolToObject(root, "wsConnected", ws_conn);
        cJSON_AddBoolToObject(root, "playerReady", ready);
        cJSON_AddStringToObject(root, "player", "zeroplay-v4l2");
        cJSON_AddStringToObject(root, "version", ZEROPLAY_VERSION);

        char *body = cJSON_PrintUnformatted(root);
        int body_len = body ? (int)strlen(body) : 0;

        /* Send HTTP headers */
        unsigned char headers[512];
        unsigned char *p = headers;
        unsigned char *end = headers + sizeof(headers);

        if (lws_add_http_header_status(wsi, (unsigned int)status, &p, end))
            goto health_done;
        if (lws_add_http_header_by_token(wsi, WSI_TOKEN_HTTP_CONTENT_TYPE,
                (const unsigned char *)"application/json", 16, &p, end))
            goto health_done;
        if (lws_add_http_header_content_length(wsi, (lws_filepos_t)body_len, &p, end))
            goto health_done;
        if (lws_finalize_http_header(wsi, &p, end))
            goto health_done;

        lws_write(wsi, headers, (size_t)(p - headers), LWS_WRITE_HTTP_HEADERS);

        /* Send body */
        if (body && body_len > 0)
            lws_write(wsi, (unsigned char *)body, (size_t)body_len, LWS_WRITE_HTTP);

health_done:
        if (body) free(body);
        cJSON_Delete(root);

        if (lws_http_transaction_completed(wsi))
            return -1;
        return 0;
    }

    default:
        break;
    }
    return lws_callback_http_dummy(wsi, reason, user, in, len);
}

/* ------------------------------------------------------------------ */
/* WebSocket thread                                                    */
/* ------------------------------------------------------------------ */

static const struct lws_protocols protocols[] = {
    { "ws-client",    callback_ws_client, 0, MAX_SEND_BUF, 0, NULL, 0 },
    { "http-health",  callback_health,    0, 0,            0, NULL, 0 },
    { NULL,           NULL,               0, 0,            0, NULL, 0 },
};

static const struct lws_http_mount health_mount = {
    .mount_next        = NULL,
    .mountpoint        = "/",
    .mountpoint_len    = 1,
    .origin_protocol   = LWSMPRO_CALLBACK,
    .protocol          = "http-health",
};

static void *ws_thread_fn(void *arg)
{
    WsContext *ctx = (WsContext *)arg;

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port      = ctx->cfg.health_port;
    info.protocols = protocols;
    info.mounts    = &health_mount;
    info.user      = ctx;
    info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
                     LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

    /* Suppress most lws logs */
    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);

    ctx->lws_ctx = lws_create_context(&info);
    if (!ctx->lws_ctx) {
        fprintf(stderr, "ws: failed to create lws context\n");
        return NULL;
    }

    /* Initiate first connection */
    sul_reconnect_cb(&ctx->sul_reconnect);

    /* Event loop */
    while (ctx->running) {
        lws_service(ctx->lws_ctx, 100);
    }

    /* Cleanup */
    lws_sul_cancel(&ctx->sul_state);
    lws_sul_cancel(&ctx->sul_ping);
    lws_sul_cancel(&ctx->sul_reconnect);
    lws_context_destroy(ctx->lws_ctx);
    ctx->lws_ctx = NULL;

    fprintf(stderr, "ws: thread exited\n");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

WsContext *ws_create(const WsConfig *cfg, WsCmdQueue *cmd_queue,
                     WsSharedState *shared)
{
    WsContext *ctx = calloc(1, sizeof(WsContext));
    if (!ctx) return NULL;

    ctx->cfg          = *cfg;
    ctx->cmd_queue    = cmd_queue;
    ctx->shared_state = shared;
    ctx->reconnect_delay_ms = RECONNECT_MIN_MS;

    if (parse_ws_url(ctx, cfg->backend_ws_url) < 0) {
        free(ctx);
        return NULL;
    }

    /* Gather device info */
    gethostname(ctx->hostname, sizeof(ctx->hostname));
    struct utsname uts;
    if (uname(&uts) == 0)
        snprintf(ctx->arch, sizeof(ctx->arch), "%s", uts.machine);
    else
        snprintf(ctx->arch, sizeof(ctx->arch), "unknown");

    return ctx;
}

int ws_start(WsContext *ctx)
{
    ctx->running = 1;
    if (pthread_create(&ctx->thread, NULL, ws_thread_fn, ctx) != 0) {
        fprintf(stderr, "ws: failed to create thread\n");
        return -1;
    }
    return 0;
}

int ws_is_connected(WsContext *ctx)
{
    return ctx->connected;
}

void ws_stop(WsContext *ctx)
{
    ctx->running = 0;
    if (ctx->lws_ctx)
        lws_cancel_service(ctx->lws_ctx);
}

void ws_destroy(WsContext *ctx)
{
    if (!ctx) return;
    ws_stop(ctx);
    pthread_join(ctx->thread, NULL);
    free(ctx);
}
