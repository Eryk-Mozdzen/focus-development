#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <stddef.h>

#include <lwip/arch.h>
#include <lwip/err.h>
#include <lwip/tcp.h>

typedef enum {
    WEBSOCKET_STATE_CONNECTING,
    WEBSOCKET_STATE_OPEN,
    WEBSOCKET_STATE_CLOSING,
    WEBSOCKET_STATE_CLOSED,
} websocket_state_t;

struct websocket_server;

struct websocket_client {
    struct websocket_client *next;
    struct websocket_client *prev;
    struct websocket_server *server;
    struct tcp_pcb *pcb;
    websocket_state_t state;
};

typedef void (*websocket_on_open_fn)(void *, struct websocket_client *);
typedef void (*websocket_on_close_fn)(void *, struct websocket_client *);
typedef void (*websocket_on_message_fn)(void *, struct websocket_client *, const void *, u32_t);

typedef struct websocket_server {
    struct tcp_pcb *pcb;
    struct websocket_client *clients;

    websocket_on_open_fn on_open;
    websocket_on_close_fn on_close;
    websocket_on_message_fn on_message;
    void *arg;
} websocket_server_t;

websocket_server_t *websocket_server_new(u16_t port);

void websocket_arg(websocket_server_t *wss, void *arg);
void websocket_on_open(websocket_server_t *wss, websocket_on_open_fn on_open);
void websocket_on_close(websocket_server_t *wss, websocket_on_close_fn on_close);
void websocket_on_message(websocket_server_t *wss, websocket_on_message_fn on_message);

err_t websocket_send(struct websocket_client *client, const void *message, u32_t message_len);
err_t websocket_close(struct websocket_client *client);

#endif
