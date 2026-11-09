// PPP over Serial (PPPoS) transport adapter for Mongoose's built-in TCP/IP.
// Include mongoose.h before this file with MG_ENABLE_TCPIP=1.
#ifndef MG_PPPOS_H
#define MG_PPPOS_H

#include "mongoose.h"

#if MG_ENABLE_TCPIP

#ifndef MG_PPPOS_MAX_FRAME
#define MG_PPPOS_MAX_FRAME 2048
#endif

#ifndef MG_PPPOS_RX_SLOTS
#define MG_PPPOS_RX_SLOTS 4
#endif

#ifndef MG_PPPOS_RESTART_INTERVAL
#define MG_PPPOS_RESTART_INTERVAL 5
#endif

// The write callback must write the whole buffer or return fewer bytes. It is
// called from Mongoose's TCP/IP driver transmit path and must not block.
typedef size_t (*mg_pppos_write_fn)(const void *data, size_t len, void *arg);

// Optional UART/device initialization callback.
typedef bool (*mg_pppos_init_fn)(void *arg);

struct mg_pppos;

// Optional serial poll callback, called from the TCP/IP driver poll.
typedef void (*mg_pppos_poll_fn)(void *arg, struct mg_pppos *state);

// Optional physical-link query. If NULL, the link is considered up.
typedef bool (*mg_pppos_link_fn)(void *arg);

// Caller-owned PPPoS state. Zero-initialize, then call mg_pppos_init_state().
struct mg_pppos {
  struct mg_tcpip_driver driver;
  mg_pppos_write_fn write;
  mg_pppos_init_fn init;
  mg_pppos_poll_fn poll;
  mg_pppos_link_fn link;
  void *arg;
  uint16_t restart_id;
  uint16_t restart_timer;
  uint16_t restart_interval;
  uint8_t rx[MG_PPPOS_RX_SLOTS][MG_PPPOS_MAX_FRAME];
  size_t rx_len[MG_PPPOS_RX_SLOTS];
  size_t rx_head, rx_tail, rx_count;
  uint8_t frame[MG_PPPOS_MAX_FRAME];
  uint8_t tx[2 * MG_PPPOS_MAX_FRAME + 2];
  size_t frame_len;
  bool escaped, in_frame, overflow, restart_complete, restart_recovery;
  uint32_t rx_bad_fcs, rx_overflow, rx_dropped;
};

// Set up the serial driver. Call before mg_tcpip_init(); assign
// ifp->driver = mg_pppos_driver(state), ifp->driver_data = state,
// ifp->l2type = MG_TCPIP_L2_PPP, and a nonzero local/peer IP configuration as
// appropriate for the existing Mongoose PPP implementation.
void mg_pppos_init_state(struct mg_pppos *state, mg_pppos_write_fn write,
                         mg_pppos_init_fn init, mg_pppos_poll_fn poll,
                         mg_pppos_link_fn link, void *arg);
#define mg_pppos_driver(state) (&(state)->driver)

// Send an LCP Configure-Request periodically while PPP negotiation is pending,
// prompting a peer with a stale session to renegotiate. Set to zero to disable.
// Defaults to MG_PPPOS_RESTART_INTERVAL seconds.
void mg_pppos_set_restart_interval(struct mg_pppos *state, uint16_t seconds);

// Feed arbitrary chunks from the UART receive path. Chunks may end mid-frame.
// The driver consumes queued complete frames on later TCP/IP polling passes.
void mg_pppos_input(struct mg_pppos *state, const void *data, size_t len);

#endif  // MG_ENABLE_TCPIP
#endif  // MG_PPPOS_H
