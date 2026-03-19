#ifndef WS_H
#define WS_H

#include <stdint.h>
#include <pthread.h>

/* ------------------------------------------------------------------ */
/* Command types (ws_thread → main_thread)                             */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_NONE = 0,
    CMD_LOAD,
    CMD_PLAY,
    CMD_PAUSE,
    CMD_STOP,
    CMD_SEEK,
} CmdType;

typedef struct {
    CmdType  type;
    char     url[2048];          /* CMD_LOAD: media URL */
    int64_t  seek_position_ms;   /* CMD_SEEK: target in milliseconds */
} WsCommand;

/* ------------------------------------------------------------------ */
/* Non-blocking command queue                                          */
/* ------------------------------------------------------------------ */

#define WS_CMD_QUEUE_SIZE 16

typedef struct {
    WsCommand       items[WS_CMD_QUEUE_SIZE];
    int             head;
    int             tail;
    int             count;
    pthread_mutex_t mutex;
} WsCmdQueue;

void ws_cmd_queue_init(WsCmdQueue *q);
int  ws_cmd_queue_push(WsCmdQueue *q, const WsCommand *cmd);  /* 1=ok, 0=full */
int  ws_cmd_queue_pop(WsCmdQueue *q, WsCommand *cmd);         /* 1=got, 0=empty */
void ws_cmd_queue_destroy(WsCmdQueue *q);

/* ------------------------------------------------------------------ */
/* Shared player state (main_thread → ws_thread, read-only for ws)    */
/* ------------------------------------------------------------------ */

typedef struct {
    pthread_mutex_t mutex;
    double          position_s;     /* playback position in seconds, -1 if unknown */
    int             paused;
    char            url[2048];      /* currently loaded URL, "" if idle */
    int             idle;           /* 1 when no media is loaded */
    int             player_ready;   /* 1 when V4L2 pipeline is initialized */
} WsSharedState;

void ws_shared_state_init(WsSharedState *s);
void ws_shared_state_destroy(WsSharedState *s);

/* Setters (called by main thread) */
void ws_shared_state_set_position(WsSharedState *s, double pos_s);
void ws_shared_state_set_paused(WsSharedState *s, int paused);
void ws_shared_state_set_url(WsSharedState *s, const char *url);
void ws_shared_state_set_idle(WsSharedState *s, int idle);
void ws_shared_state_set_player_ready(WsSharedState *s, int ready);

/* ------------------------------------------------------------------ */
/* WebSocket configuration                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *backend_ws_url;     /* ws://host:port/path */
    const char *device_token;
    int         health_port;        /* default 3000 */
    int         state_interval_ms;  /* default 5000 */
    int         ping_interval_ms;   /* default 20000 */
} WsConfig;

/* ------------------------------------------------------------------ */
/* WebSocket context (opaque)                                          */
/* ------------------------------------------------------------------ */

typedef struct WsContext WsContext;

WsContext *ws_create(const WsConfig *cfg, WsCmdQueue *cmd_queue,
                     WsSharedState *shared);
int        ws_start(WsContext *ctx);          /* spawns thread, returns 0 or -1 */
int        ws_is_connected(WsContext *ctx);   /* 1 if WS open */
void       ws_stop(WsContext *ctx);           /* signals thread to exit */
void       ws_destroy(WsContext *ctx);        /* joins thread, frees all */

#endif
