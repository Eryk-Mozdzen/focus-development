// PPPoS serial framing adapter for Mongoose's built-in PPP L2.
// Implements the asynchronous HDLC-like framing from RFC 1662.
#include "mg_pppos.h"

#if MG_ENABLE_TCPIP

#define PPP_FLAG 0x7e
#define PPP_ESC 0x7d
#define PPP_TRANS 0x20
#define PPP_INITFCS 0xffff
#define PPP_GOODFCS 0xf0b8

static uint16_t ppp_fcs(uint16_t fcs, uint8_t byte) {
  unsigned i;
  fcs ^= byte;
  for (i = 0; i < 8; i++)
    fcs = (uint16_t) ((fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1);
  return fcs;
}

static bool pppos_init(struct mg_tcpip_if *ifp) {
  struct mg_pppos *s = (struct mg_pppos *) ifp->driver_data;
  return s != NULL && s->write != NULL && (s->init == NULL || s->init(s->arg));
}

static size_t pppos_tx(const void *data, size_t len, struct mg_tcpip_if *ifp) {
  struct mg_pppos *s = (struct mg_pppos *) ifp->driver_data;
  const uint8_t *src = (const uint8_t *) data;
  uint8_t *out;
  size_t i, n = 0;
  if (s == NULL || s->write == NULL || len > MG_PPPOS_MAX_FRAME) return 0;
  out = s->tx;
  out[n++] = PPP_FLAG;
  for (i = 0; i < len; i++) {
    uint8_t c = src[i];
    if (c == PPP_FLAG || c == PPP_ESC || c < 0x20) {
      out[n++] = PPP_ESC;
      out[n++] = (uint8_t) (c ^ PPP_TRANS);
    } else {
      out[n++] = c;
    }
  }
  out[n++] = PPP_FLAG;
  return s->write(out, n, s->arg) == n ? len : 0;
}

static size_t pppos_rx(void *buf, size_t len, struct mg_tcpip_if *ifp) {
  struct mg_pppos *s = (struct mg_pppos *) ifp->driver_data;
  size_t n;
  if (s == NULL || s->rx_count == 0) return 0;
  n = s->rx_len[s->rx_head];
  if (n > len) {
    s->rx_dropped++;
  } else {
    memcpy(buf, s->rx[s->rx_head], n);
  }
  s->rx_head = (s->rx_head + 1) % MG_PPPOS_RX_SLOTS;
  s->rx_count--;
  return n > len ? 0 : n;
}

static bool pppos_poll(struct mg_tcpip_if *ifp, bool expired_1000ms) {
  struct mg_pppos *s = (struct mg_pppos *) ifp->driver_data;
  (void) expired_1000ms;
  return s != NULL && (s->link == NULL || s->link(s->arg));
}

static void pppos_finish_frame(struct mg_pppos *s) {
  size_t i;
  uint16_t fcs = PPP_INITFCS;
  if (s->overflow) {
    s->rx_overflow++;
  } else if (s->frame_len >= 4) {
    for (i = 0; i < s->frame_len; i++) fcs = ppp_fcs(fcs, s->frame[i]);
    if (fcs == PPP_GOODFCS) {
      size_t slot;
      if (s->rx_count < MG_PPPOS_RX_SLOTS) {
        slot = s->rx_tail;
        memcpy(s->rx[slot], s->frame, s->frame_len);
        s->rx_len[slot] = s->frame_len;
        s->rx_tail = (s->rx_tail + 1) % MG_PPPOS_RX_SLOTS;
        s->rx_count++;
      } else {
        s->rx_dropped++;
      }
    } else {
      s->rx_bad_fcs++;
    }
  }
  s->frame_len = 0;
  s->overflow = false;
  s->escaped = false;
}

void mg_pppos_input(struct mg_pppos *s, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *) data;
  size_t i;
  if (s == NULL || (p == NULL && len != 0)) return;
  for (i = 0; i < len; i++) {
    uint8_t c = p[i];
    if (c == PPP_FLAG) {
      if (s->in_frame && (s->frame_len != 0 || s->overflow))
        pppos_finish_frame(s);
      s->in_frame = true;
      continue;
    }
    if (!s->in_frame || s->overflow) continue;
    if (s->escaped) {
      c ^= PPP_TRANS;
      s->escaped = false;
    } else if (c == PPP_ESC) {
      s->escaped = true;
      continue;
    }
    if (s->frame_len == MG_PPPOS_MAX_FRAME) {
      s->overflow = true;
    } else {
      s->frame[s->frame_len++] = c;
    }
  }
}

void mg_pppos_init_state(struct mg_pppos *s, mg_pppos_write_fn write,
                         mg_pppos_init_fn init, mg_pppos_link_fn link,
                         void *arg) {
  if (s == NULL) return;
  memset(s, 0, sizeof(*s));
  s->write = write;
  s->init = init;
  s->link = link;
  s->arg = arg;
  s->driver.init = pppos_init;
  s->driver.tx = pppos_tx;
  s->driver.rx = pppos_rx;
  s->driver.poll = pppos_poll;
}

struct mg_tcpip_driver *mg_pppos_driver(struct mg_pppos *s) {
  return s == NULL ? NULL : &s->driver;
}

#endif  // MG_ENABLE_TCPIP
