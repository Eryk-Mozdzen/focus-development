// Linux serial-device example for the Mongoose PPPoS adapter.
// Build with MG_ENABLE_TCPIP=1 and MG_ENABLE_LWIP=0.
#include "mg_pppos.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

static volatile sig_atomic_t s_running = 1;
static int s_serial = -1;
static struct mg_pppos s_pppos;

static void stop(int signo) {
  (void) signo;
  s_running = 0;
}

static bool serial_init(void *arg) {
  (void) arg;
  return s_serial >= 0;
}

static bool serial_link(void *arg) {
  (void) arg;
  return s_serial >= 0;
}

static size_t serial_write(const void *data, size_t len, void *arg) {
  const uint8_t *p = (const uint8_t *) data;
  size_t sent = 0;
  (void) arg;
  while (sent < len) {
    ssize_t n = write(s_serial, p + sent, len - sent);
    if (n > 0) {
      sent += (size_t) n;
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd fd = {s_serial, POLLOUT, 0};
      if (poll(&fd, 1, 1000) <= 0) break;
    } else {
      break;
    }
  }
  return sent;
}

static bool serial_open(const char *path) {
  struct termios tio;
  s_serial = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (s_serial < 0 || tcgetattr(s_serial, &tio) != 0) return false;
  cfmakeraw(&tio);
  cfsetispeed(&tio, B115200);
  cfsetospeed(&tio, B115200);
  tio.c_cflag |= CLOCAL | CREAD;
  return tcsetattr(s_serial, TCSANOW, &tio) == 0;
}

static void handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "PPPoS is up\n");
  }
  (void) ev_data;
}

int main(int argc, char *argv[]) {
  struct mg_mgr mgr;
  struct mg_tcpip_if netif;
  struct mg_addr ip, gw;
  uint8_t buf[256];
  if (argc != 2) {
    fprintf(stderr, "Usage: %s /dev/ttyXXX\n", argv[0]);
    return 2;
  }
  if (!serial_open(argv[1])) {
    perror("open/configure serial device");
    return 1;
  }
  signal(SIGINT, stop);
  signal(SIGTERM, stop);
  mg_mgr_init(&mgr);
  memset(&netif, 0, sizeof(netif));
  mg_aton(mg_str("192.0.2.2"), &ip);
  mg_aton(mg_str("192.0.2.1"), &gw);
  netif.ip = ip.addr.ip4;
  netif.gw = gw.addr.ip4;
  netif.mask = mg_htonl(0xffffffff);
  netif.l2type = MG_TCPIP_L2_PPP;
  netif.enable_dhcp_client = false;
  mg_pppos_init_state(&s_pppos, serial_write, serial_init, serial_link, NULL);
  netif.driver = mg_pppos_driver(&s_pppos);
  netif.driver_data = &s_pppos;
  mg_tcpip_init(&mgr, &netif);
  mg_http_listen(&mgr, "tcp://0.0.0.0:8080", handler, NULL);
  MG_INFO(("PPPoS on %s; HTTP will answer on port 8080 after PPP comes up",
           argv[1]));
  while (s_running) {
    ssize_t n = read(s_serial, buf, sizeof(buf));
    if (n > 0) mg_pppos_input(&s_pppos, buf, (size_t) n);
    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
             errno != EINTR) {
      perror("read serial device");
      break;
    }
    mg_mgr_poll(&mgr, 1);
  }
  mg_mgr_free(&mgr);
  close(s_serial);
  return 0;
}
