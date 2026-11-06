// TODO: handle fragmented messages
// TODO: handle payload length 7+16
// TODO: handle payload length 7+64

#include <stddef.h>
#include <string.h>

#include <lwip/arch.h>
#include <lwip/err.h>
#include <lwip/mem.h>
#include <lwip/pbuf.h>
#include <lwip/tcp.h>
#include <netif/ppp/polarssl/sha1.h>

#include "websocket.h"

typedef enum {
    OPCODE_CONTINUATION = 0,
    OPCODE_TEXT = 1,
    OPCODE_BINARY = 2,
    OPCODE_CLOSE = 8,
    OPCODE_PING = 9,
    OPCODE_PONG = 10,
} opcode_t;

static void request_get_key(const char *request, const u32_t request_len, char key[24]) {
    const char header[] = "Sec-WebSocket-Key:";
    const u32_t header_len = sizeof(header) - 1;

    u32_t header_pos = 0;

    while(header_pos < request_len) {
        u32_t line_end = header_pos;

        while((line_end < request_len) && (request[line_end] != '\n') &&
              (request[line_end] != '\r')) {
            line_end++;
        }

        const u32_t line_len = line_end - header_pos;

        if((line_len >= header_len) && (memcmp(request + header_pos, header, header_len) == 0)) {
            u32_t value_pos = header_pos + header_len;

            while((value_pos < line_end) &&
                  ((request[value_pos] == ' ') || (request[value_pos] == '\t'))) {
                value_pos++;
            }

            if((line_end - value_pos) != 24) {
                return;
            }

            memcpy(key, request + value_pos, 24);
        }

        while((line_end < request_len) &&
              ((request[line_end] == '\r') || (request[line_end] == '\n'))) {
            line_end++;
        }

        header_pos = line_end;
    }
}

static u32_t base64_encode(char *dest, const u8_t *input, const u32_t input_length) {
    const char encoding_table[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
        'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/',
    };

    const u32_t mod_table[] = {0, 2, 1};

    const u32_t dest_length = 4 * ((input_length + 2) / 3);

    for(u32_t i = 0, j = 0; i < input_length;) {
        const u32_t octet_a = (i < input_length) ? input[i++] : 0;
        const u32_t octet_b = (i < input_length) ? input[i++] : 0;
        const u32_t octet_c = (i < input_length) ? input[i++] : 0;

        const u32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        dest[j++] = encoding_table[(triple >> (3 * 6)) & 0x3F];
        dest[j++] = encoding_table[(triple >> (2 * 6)) & 0x3F];
        dest[j++] = encoding_table[(triple >> (1 * 6)) & 0x3F];
        dest[j++] = encoding_table[(triple >> (0 * 6)) & 0x3F];
    }

    for(u32_t i = 0; i < mod_table[input_length % 3]; i++) {
        dest[dest_length - 1 - i] = '=';
    }

    return dest_length;
}

static void payload_unmask(u8_t *output,
                           const u8_t *input,
                           const u32_t input_length,
                           const u8_t masking_key[4]) {
    for(u32_t i = 0; i < input_length; i++) {
        output[i] = input[i] ^ masking_key[i % 4];
    }
}

static err_t connection_cleanup(struct websocket_client *client) {
    const err_t err = tcp_close(client->pcb);

    if(client->next != NULL) {
        client->next->prev = client->prev;
    }

    if(client->prev != NULL) {
        client->prev->next = client->next;
    } else {
        client->server->clients = client->next;
    }

    mem_free(client);

    return err;
}

static err_t websocket_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct websocket_client *client = arg;
    websocket_server_t *wss = client->server;

    if(p == NULL) {
        return connection_cleanup(client);
    }

    tcp_recved(pcb, p->tot_len);

    const u8_t *data = p->payload;

    switch(client->state) {
        case WEBSOCKET_STATE_CONNECTING: {
            char key[24] = {0};

            request_get_key(p->payload, p->tot_len, key);

            if(key[0] != 0) {
                const char *magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

                unsigned char sha1[20] = {0};
                sha1_context sha1_ctx;
                sha1_starts(&sha1_ctx);
                sha1_update(&sha1_ctx, (unsigned char *)key, sizeof(key));
                sha1_update(&sha1_ctx, (unsigned char *)magic, 36);
                sha1_finish(&sha1_ctx, sha1);

                char response[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Sec-WebSocket-Accept: ????????????????????????????\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Upgrade: websocket\r\n"
                                  "\r\n";

                base64_encode(&response[56], sha1, sizeof(sha1));

                tcp_write(pcb, response, strlen(response), TCP_WRITE_FLAG_COPY);
                tcp_output(pcb);
                client->state = WEBSOCKET_STATE_OPEN;
            }
        } break;
        case WEBSOCKET_STATE_OPEN: {
            u8_t payload[125] = {0};

            const opcode_t opcode = (data[0] & 0x0F);
            const u8_t masked = (data[1] & 0x80);
            const u8_t payload_len = (data[1] & 0x7F);

            switch(opcode) {
                case OPCODE_CONTINUATION: {

                } break;
                case OPCODE_TEXT:
                case OPCODE_BINARY: {
                    if(payload_len <= 125) {
                        if(masked) {
                            payload_unmask(payload, &data[6], payload_len, &data[2]);
                        } else {
                            memcpy(payload, &data[2], payload_len);
                        }

                        if(wss->on_message != NULL) {
                            wss->on_message(wss->arg, client, payload, payload_len);
                        }
                    }
                } break;
                case OPCODE_CLOSE: {
                    if(payload_len <= 125) {
                        client->state = WEBSOCKET_STATE_CLOSED;
                        if(payload_len >= 2) {
                            u8_t frame[4];
                            frame[0] = 0x80 | ((OPCODE_CLOSE << 0) & 0x0F);
                            frame[1] = ((2 << 0) & 0x7F);
                            if(masked) {
                                payload_unmask(&frame[2], &data[6], 2, &data[2]);
                            } else {
                                memcpy(&frame[2], &data[2], 2);
                            }
                            tcp_write(client->pcb, frame, sizeof(frame), TCP_WRITE_FLAG_COPY);
                        } else {
                            u8_t frame[2];
                            frame[0] = 0x80 | ((OPCODE_CLOSE << 0) & 0x0F);
                            frame[1] = ((0 << 0) & 0x7F);
                            tcp_write(client->pcb, frame, sizeof(frame), TCP_WRITE_FLAG_COPY);
                        }
                        tcp_output(client->pcb);
                        connection_cleanup(client);
                    }
                } break;
                case OPCODE_PING: {
                    if(payload_len <= 125) {
                        u8_t frame[256];
                        frame[0] = 0x80 | ((OPCODE_PONG << 0) & 0x0F);
                        frame[1] = ((payload_len << 0) & 0x7F);
                        if(masked) {
                            payload_unmask(&frame[2], &data[6], payload_len, &data[2]);
                        } else {
                            memcpy(&frame[2], &data[2], payload_len);
                        }
                        tcp_write(client->pcb, frame, 2 + payload_len, TCP_WRITE_FLAG_COPY);
                        tcp_output(client->pcb);
                    }
                } break;
                case OPCODE_PONG: {

                } break;
            }
        } break;
        case WEBSOCKET_STATE_CLOSING: {
            const opcode_t opcode = (data[0] & 0x0F);

            if(opcode == OPCODE_CLOSE) {
                if(wss->on_close != NULL) {
                    wss->on_close(wss->arg, client);
                }
                client->state = WEBSOCKET_STATE_CLOSED;
                connection_cleanup(client);
            }
        } break;
        case WEBSOCKET_STATE_CLOSED: {

        } break;
    }

    pbuf_free(p);

    return ERR_OK;
}

static err_t websocket_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    websocket_server_t *wss = arg;

    struct websocket_client *client = mem_malloc(sizeof(struct websocket_client));
    client->server = wss;
    client->pcb = newpcb;
    client->state = WEBSOCKET_STATE_CONNECTING;
    tcp_arg(client->pcb, client);
    tcp_recv(client->pcb, websocket_recv);

    if(wss->clients != NULL) {
        wss->clients->prev = client;
    }
    client->next = wss->clients;
    client->prev = NULL;
    wss->clients = client;

    if(wss->on_open != NULL) {
        wss->on_open(wss->arg, client);
    }

    return ERR_OK;
}

websocket_server_t *websocket_server_new(u16_t port) {
    websocket_server_t *wss = mem_malloc(sizeof(websocket_server_t));

    if(wss == NULL) {
        return wss;
    }

    wss->pcb = tcp_new();
    wss->clients = NULL;
    wss->on_open = NULL;
    wss->on_close = NULL;
    wss->on_message = NULL;
    wss->arg = NULL;

    if(wss->pcb == NULL) {
        mem_free(wss);
        return NULL;
    }

    if(tcp_bind(wss->pcb, IP_ADDR_ANY, port) != ERR_OK) {
        tcp_close(wss->pcb);
        mem_free(wss);
        return NULL;
    }

    wss->pcb = tcp_listen(wss->pcb);

    tcp_arg(wss->pcb, wss);
    tcp_accept(wss->pcb, websocket_accept);

    return wss;
}

void websocket_arg(websocket_server_t *wss, void *arg) {
    if(wss != NULL) {
        wss->arg = arg;
    }
}

void websocket_on_open(websocket_server_t *wss, websocket_on_open_fn on_open) {
    if(wss != NULL) {
        wss->on_open = on_open;
    }
}

void websocket_on_close(websocket_server_t *wss, websocket_on_close_fn on_close) {
    if(wss != NULL) {
        wss->on_close = on_close;
    }
}

void websocket_on_message(websocket_server_t *wss, websocket_on_message_fn on_message) {
    if(wss != NULL) {
        wss->on_message = on_message;
    }
}

err_t websocket_send(struct websocket_client *client, const void *message, u32_t message_len) {
    if(client->state != WEBSOCKET_STATE_OPEN) {
        return ERR_CONN;
    }

    if(message_len > 125) {
        message_len = 125;
    }

    u8_t frame[256];
    frame[0] = 0x80 | ((OPCODE_BINARY << 0) & 0x0F);
    frame[1] = ((message_len << 0) & 0x7F);
    memcpy(&frame[2], message, message_len);

    err_t err;
    err = tcp_write(client->pcb, frame, 2 + message_len, TCP_WRITE_FLAG_COPY);
    if(err != ERR_OK) {
        return err;
    }
    err = tcp_output(client->pcb);
    if(err != ERR_OK) {
        return err;
    }

    return ERR_OK;
}

err_t websocket_close(struct websocket_client *client) {
    if(client->state != WEBSOCKET_STATE_OPEN) {
        return ERR_CONN;
    }

    client->state = WEBSOCKET_STATE_CLOSING;

    const uint16_t status_code = 1000;

    u8_t frame[4];
    frame[0] = 0x80 | ((OPCODE_CLOSE << 0) & 0x0F);
    frame[1] = ((2 << 0) & 0x7F);
    frame[2] = (status_code >> 8) & 0xFF;
    frame[3] = (status_code >> 0) & 0xFF;

    err_t err;
    err = tcp_write(client->pcb, frame, sizeof(frame), TCP_WRITE_FLAG_COPY);
    if(err != ERR_OK) {
        return err;
    }
    err = tcp_output(client->pcb);
    if(err != ERR_OK) {
        return err;
    }

    return ERR_OK;
}
