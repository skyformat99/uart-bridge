/*
 * Copyright (c) 2014-2018 Cesanta Software Limited
 * All rights reserved
 */

#include "uart_bridge.h"
#include "bt_svc.h"

#include <stdbool.h>

#include "common/cs_dbg.h"

#include "mgos_app.h"
#include "mgos_hal.h"
#include "mgos_mongoose.h"
#include "mgos_timers.h"
#include "mgos_sys_config.h"
#include "mgos_uart.h"
#include "mgos_utils.h"

#if CS_PLATFORM == CS_P_ESP8266
#include "user_interface.h"
#include "fw/platforms/esp8266/src/esp_uart.h"
#elif CS_PLATFORM == CS_P_ESP32
#include "fw/platforms/esp32/src/esp32_uart.h"
#elif CS_PLATFORM == CS_P_CC3200 || CS_PLATFORM == CS_P_CC3220
#include "inc/hw_types.h"
#include "driverlib/uart.h"
#include "cc32xx_uart.h"
#else
#error Unsupported platform
#endif

#ifdef MGOS_HAVE_PWM
#include "mgos_pwm.h"
#else
bool mgos_pwm_set(int pin, int freq, float duty) {
  (void) pin;
  (void) freq;
  (void) duty;
  return false;
}
#endif

#ifndef IRAM
#define IRAM
#endif

static const struct mgos_config_tcp *s_tcfg = NULL;
static const struct mgos_config_uart *s_ucfg = NULL;
static const struct mgos_config_misc *s_mcfg = NULL;

static struct tu_conn s_tu_conn = {
    .type = TU_CONN_TYPE_NONE,
};
static struct mg_connection *s_client_conn = NULL;
static struct mg_connection *s_listener_conn = NULL;
static double s_last_connect_attempt = 0;
static struct mbuf s_tcp_rx_tail;
static double s_last_activity = 0;
static double s_last_tcp_status_report = 0;
static double s_last_uart_status_report = 0;
static struct mgos_uart_stats s_prev_stats;

static struct mbuf s_bt_buf_rx;

static void tu_conn_mgr(struct mg_connection *nc, int ev, void *ev_data,
                        void *user_data);
static void tu_conn_mgr_timer_cb(void *arg);
static void tu_tcp_conn_handler(struct mg_connection *nc, int ev, void *ev_data,
                                void *user_data);
static void tu_ws_conn_handler(struct mg_connection *nc, int ev, void *ev_data,
                               void *user_data);
static IRAM void tu_dispatcher(int uart_no, void *arg);

tu_uart_processor_fn tu_uart_processor;

static char *stringify_tu_conn(const struct tu_conn *c) {
  /* TODO(dfrank) stringify the connection properly */
  (void) c;
  return "conn";
}

static bool init_tcp(const struct mgos_config_tcp *cfg) {
  char listener_spec[100];
  struct mg_bind_opts bopts;
  memset(&bopts, 0, sizeof(bopts));
  if (cfg->listener.port > 0) {
    sprintf(listener_spec, "%d", cfg->listener.port);
    if (cfg->listener.tls.cert) {
      bopts.ssl_cert = cfg->listener.tls.cert;
    }
    LOG(LL_INFO, ("Listening on %s (%s, %s)", listener_spec,
                  (cfg->listener.ws.enable ? "WS" : "no WS"),
                  (bopts.ssl_cert ? bopts.ssl_cert : "no SSL")));
    s_listener_conn =
        mg_bind_opt(mgos_get_mgr(), listener_spec, tu_conn_mgr, NULL, bopts);
    if (s_listener_conn == NULL) {
      LOG(LL_ERROR, ("Failed to create listener"));
      return 0;
    }
    if (cfg->listener.ws.enable) {
      mg_set_protocol_http_websocket(s_listener_conn);
    }
  }
  mgos_set_timer(200 /* ms */, true /* repeat */, tu_conn_mgr_timer_cb, NULL);
  s_tcfg = cfg;
  return true;
}

static bool init_uart(const struct mgos_config_uart *ucfg) {
  if (ucfg->uart_no < 0) {
    LOG(LL_INFO, ("UART is disabled"));
    return true;
  }
  struct mgos_uart_config cfg;
  mgos_uart_config_set_defaults(ucfg->uart_no, &cfg);
  cfg.baud_rate = ucfg->baud_rate;
  cfg.rx_buf_size = ucfg->rx_buf_size;
  cfg.rx_fc_type = (ucfg->rx_fc_ena ? MGOS_UART_FC_HW : MGOS_UART_FC_NONE);
  cfg.rx_linger_micros = ucfg->rx_linger_micros;
  cfg.tx_buf_size = ucfg->tx_buf_size;
  cfg.tx_fc_type = (ucfg->tx_fc_ena ? MGOS_UART_FC_HW : MGOS_UART_FC_NONE);
#if CS_PLATFORM == CS_P_ESP32 || CS_PLATFORM == CS_P_ESP8266
  cfg.dev.rx_fifo_full_thresh = ucfg->rx_fifo_full_thresh;
  cfg.dev.rx_fifo_fc_thresh = ucfg->rx_fifo_fc_thresh;
  cfg.dev.rx_fifo_alarm = ucfg->rx_fifo_alarm;
  cfg.dev.tx_fifo_empty_thresh = ucfg->tx_fifo_empty_thresh;
#if CS_PLATFORM == CS_P_ESP8266
  cfg.dev.swap_rxcts_txrts = ucfg->swap_rxcts_txrts;
#else
  if (ucfg->rx_gpio >= 0) cfg.dev.rx_gpio = ucfg->rx_gpio;
  if (ucfg->tx_gpio >= 0) cfg.dev.tx_gpio = ucfg->tx_gpio;
  if (ucfg->cts_gpio >= 0) cfg.dev.cts_gpio = ucfg->cts_gpio;
  if (ucfg->rts_gpio >= 0) cfg.dev.rts_gpio = ucfg->rts_gpio;
#endif
#endif
  if (!mgos_uart_configure(ucfg->uart_no, &cfg)) {
    LOG(LL_ERROR, ("UART init failed"));
    return false;
  }
  mgos_uart_set_dispatcher(ucfg->uart_no, tu_dispatcher, NULL);
  if (!ucfg->rx_throttle_when_no_net) {
    mgos_uart_set_rx_enabled(ucfg->uart_no, true);
  }
  LOG(LL_INFO, ("UART%d configured: %d fc %d/%d", ucfg->uart_no, cfg.baud_rate,
                cfg.rx_fc_type, cfg.tx_fc_type));
  s_ucfg = ucfg;
  return true;
}

size_t tu_dispatch_to_uart(struct mbuf *mb, int uart_no) {
  size_t len = 0;
  if (uart_no >= 0) {
    len = MIN(mb->len, mgos_uart_write_avail(uart_no));
    if (len > 0) {
      len = mgos_uart_write(uart_no, mb->buf, len);
      mbuf_remove(mb, len);
      mgos_uart_schedule_dispatcher(uart_no, false /* from_isr */);
    }
  } else {
    /* Dispatch to /dev/null */
    len = mb->len;
    mbuf_remove(mb, len);
  }
  return len;
}

void check_beeper(void) {
  static int beeping_on_gpio = -1;
  static double last_change = 0;
  if (beeping_on_gpio >= 0) {
    if (s_mcfg->beeper.timeout_seconds == 0 ||
        s_mcfg->beeper.gpio_no != beeping_on_gpio ||
        (mg_time() - last_change > 0.9)) {
      mgos_pwm_set(beeping_on_gpio, 0, 0);
      beeping_on_gpio = -1;
      last_change = mg_time();
      return;
    }
    /* Continue beeping. */
    return;
  }
  /* Is beeping on inactivity enabled? */
  if (s_mcfg->beeper.timeout_seconds <= 0 || s_mcfg->beeper.gpio_no < 0) {
    return;
  }
  /* Should we be beeping? */
  const double now = mg_time();
  if ((now - s_last_activity > s_mcfg->beeper.timeout_seconds) &&
      (now - last_change > 0.9)) {
    beeping_on_gpio = s_mcfg->beeper.gpio_no;
    mgos_pwm_set(beeping_on_gpio, 4000, 0.5); /* BEEEP! (4 KHz) */
    last_change = now;
    LOG(LL_WARN,
        ("No activity for %d seconds - BEEP!", (int) (now - s_last_activity)));
  }
}

static int uart_cts(int uart_no) {
#if CS_PLATFORM == CS_P_ESP8266
  return esp_uart_cts(uart_no);
#elif CS_PLATFORM == CS_P_ESP32
  return esp32_uart_cts(uart_no);
#elif CS_PLATFORM == CS_P_CC3200 || CS_P_CC3200 == CS_P_CC3220
  return cc32xx_uart_cts(uart_no);
#endif
}

static uint32_t uart_raw_ints(int uart_no) {
#if CS_PLATFORM == CS_P_ESP8266
  return esp_uart_raw_ints(uart_no);
#elif CS_PLATFORM == CS_P_ESP32
  return esp32_uart_raw_ints(uart_no);
#elif CS_PLATFORM == CS_P_CC3200 || CS_P_CC3200 == CS_P_CC3220
  return cc32xx_uart_raw_ints(uart_no);
#endif
}

static uint32_t uart_int_mask(int uart_no) {
#if CS_PLATFORM == CS_P_ESP8266
  return esp_uart_int_mask(uart_no);
#elif CS_PLATFORM == CS_P_ESP32
  return esp32_uart_int_mask(uart_no);
#elif CS_PLATFORM == CS_P_CC3200 || CS_P_CC3200 == CS_P_CC3220
  return cc32xx_uart_int_mask(uart_no);
#endif
}

int uart_rx_fifo_len(int uart_no) {
#if CS_PLATFORM == CS_P_ESP8266
  return esp_uart_rx_fifo_len(uart_no);
#elif CS_PLATFORM == CS_P_ESP32
  return esp32_uart_rx_fifo_len(uart_no);
#elif CS_PLATFORM == CS_P_CC3200 || CS_P_CC3200 == CS_P_CC3220
  /* It's not possible to get exact FIFO length on CC3200. */
  return UARTCharsAvail(cc32xx_uart_get_base(uart_no));
#endif
}

int uart_tx_fifo_len(int uart_no) {
#if CS_PLATFORM == CS_P_ESP8266
  return esp_uart_tx_fifo_len(uart_no);
#elif CS_PLATFORM == CS_P_ESP32
  return esp32_uart_tx_fifo_len(uart_no);
#elif CS_PLATFORM == CS_P_CC3200 || CS_P_CC3200 == CS_P_CC3220
  /* It's not possible to get exact FIFO length on CC3200. */
  return (UARTSpaceAvail(cc32xx_uart_get_base(uart_no)) == 0);
#endif
}

static void report_status(struct mg_connection *nc, int force) {
  int uart_no = (s_ucfg == NULL) ? -1 : s_ucfg->uart_no;
  double now = mg_time();
  if (nc != NULL && s_tcfg->status_interval_ms > 0 &&
      (force ||
       (now - s_last_tcp_status_report) * 1000 >= s_tcfg->status_interval_ms)) {
    char addr[32];
    mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr),
                        MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);
    fprintf(stderr, "TCP %p %s f %d rb %d sb %d\n", nc, addr, (int) nc->flags,
            (int) nc->recv_mbuf.len, (int) nc->send_mbuf.len);
    s_last_tcp_status_report = now;
  }
  if (uart_no >= 0 && s_ucfg->status_interval_ms > 0 &&
      (force ||
       (now - s_last_uart_status_report) * 1000 >=
           s_ucfg->status_interval_ms)) {
    const struct mgos_uart_stats *s = mgos_uart_get_stats(uart_no);
    struct mgos_uart_stats *ps = &s_prev_stats;
    fprintf(stderr,
            "UART%d ints %u/%u/%u; rx en %d bytes %u buf %u fifo %u, ovf %u, "
            "lcs %u; "
            "tx %u %u %u, thr %u; hf %u i 0x%03x ie 0x%03x cts %d\n",
            uart_no, s->ints - ps->ints, s->rx_ints - ps->rx_ints,
            s->tx_ints - ps->tx_ints, mgos_uart_is_rx_enabled(uart_no),
            s->rx_bytes - ps->rx_bytes, mgos_uart_read_avail(uart_no),
            uart_rx_fifo_len(uart_no), s->rx_overflows - ps->rx_overflows,
            s->rx_linger_conts - ps->rx_linger_conts,
            s->tx_bytes - ps->tx_bytes, mgos_uart_write_avail(uart_no),
            uart_tx_fifo_len(uart_no), s->tx_throttles - ps->tx_throttles,
            mgos_get_free_heap_size(), uart_raw_ints(uart_no),
            uart_int_mask(uart_no), uart_cts(uart_no));
    memcpy(ps, s, sizeof(*s));
    s_last_uart_status_report = now;
  }
}

static void tu_process_uart(int uart_no, struct tu_conn *tu_conn) {
  switch (s_tu_conn.type) {
    case TU_CONN_TYPE_TCP: {
      struct mg_connection *nc = tu_conn->conn;
      size_t len = 0;
      int num_sent = 0;
      while (mgos_uart_read_avail(uart_no) > 0 &&
             (len = (s_tcfg->tx_buf_size - nc->send_mbuf.len)) > 0) {
        len = MIN(len, mgos_uart_read_avail(uart_no));
        char *b = (char *) malloc(len);
        len = mgos_uart_read(uart_no, b, len);
        if (nc->flags & MG_F_IS_WEBSOCKET) {
          mg_send_websocket_frame(nc, WEBSOCKET_OP_BINARY, b, len);
        } else {
          mg_send(nc, b, len);
        }
        free(b);
        num_sent += len;
      }
      if (num_sent > 0) {
        LOG(LL_DEBUG, ("UART -> %d -> %s", num_sent,
                       (nc->flags & MG_F_IS_WEBSOCKET ? "WS" : "TCP")));
        s_last_activity = mg_time();
      }
    } break;

    case TU_CONN_TYPE_BT_GATTS:
      while (mgos_uart_read_avail(uart_no) > 0) {
        size_t len = MIN(
            mgos_uart_read_avail(uart_no),
            mgos_sys_config_get_tu_bt_tx_buf_size() - tu_bt_get_tx_buf_len());
        if (len <= 0) {
          break;
        }
        char *b = (char *) malloc(len);
        len = mgos_uart_read(uart_no, b, len);

        bool ok = tu_bt_send(tu_conn->tubc, mg_mk_str_n(b, len));
        if (ok) {
          LOG(LL_DEBUG, ("UART -> %d -> BT", len));
        }
        free(b);
      }
      break;

    case TU_CONN_TYPE_NONE:
      break;
  }
}

static IRAM void tu_dispatcher(int uart_no, void *arg) {
  /* TCP -> UART */
  /* Drain buffer left from a previous connection, if any. */
  if (s_tcp_rx_tail.len > 0) {
    LOG(LL_DEBUG,
        ("There are %d bytes in the tail, dispatching", s_tcp_rx_tail.len));
    tu_dispatch_to_uart(&s_tcp_rx_tail, uart_no);
    mbuf_trim(&s_tcp_rx_tail);
  }

  if (s_bt_buf_rx.len > 0) {
    LOG(LL_DEBUG,
        ("There are %d bytes in the bt buffer, dispatching", s_bt_buf_rx.len));
    tu_dispatch_to_uart(&s_bt_buf_rx, uart_no);
  }

  /* UART -> TCP */
  switch (s_tu_conn.type) {
    case TU_CONN_TYPE_NONE:
      /* Do nothing: will just accumulate UART data */
      break;

    case TU_CONN_TYPE_TCP:
    case TU_CONN_TYPE_BT_GATTS: {
      if (mgos_uart_read_avail(uart_no) > 0) {
        tu_uart_processor(uart_no, &s_tu_conn);
      }
    } break;
  }

  (void) arg;
}

static void close_cur_conn(void) {
  switch (s_tu_conn.type) {
    case TU_CONN_TYPE_TCP:
      s_tu_conn.conn->flags |= MG_F_SEND_AND_CLOSE;
      s_tu_conn.conn = NULL;
      break;

    case TU_CONN_TYPE_BT_GATTS:
      tu_bt_close(s_tu_conn.tubc);
      break;

    case TU_CONN_TYPE_NONE:
      /* Nothing to do */
      break;
  }
  s_tu_conn.type = TU_CONN_TYPE_NONE;
}

static void tu_tcp_conn_handler(struct mg_connection *nc, int ev, void *ev_data,
                                void *user_data) {
  (void) ev_data;
  (void) user_data;

  mgos_wdt_feed();

  switch (ev) {
    case MG_EV_POLL:
    case MG_EV_RECV: {
      /* If there is a tail from previous conn, we need it to drain first. */
      if (s_tcp_rx_tail.len > 0) break;
      /* TCP -> UART */
      size_t len = tu_dispatch_to_uart(&nc->recv_mbuf, s_ucfg->uart_no);
      if (len > 0) {
        LOG(LL_DEBUG, ("UART <- %d <- TCP", (int) len));
        s_last_activity = mg_time();
      }
      break;
    }
    case MG_EV_SEND: {
      if (s_ucfg->uart_no >= 0) {
        mgos_uart_schedule_dispatcher(s_ucfg->uart_no, false /* from_isr */);
      }
      break;
    }
    case MG_EV_CLOSE: {
      LOG(LL_INFO, ("%p closed", nc));
      report_status(nc, 1 /* force */);
      if (s_tu_conn.type == TU_CONN_TYPE_TCP && nc == s_tu_conn.conn) {
        if (s_ucfg != NULL && s_ucfg->rx_throttle_when_no_net) {
          mgos_uart_set_rx_enabled(s_ucfg->uart_no, false);
        }
        if (nc->recv_mbuf.len > 0) {
          /* Rescue the bytes remaining in the rx buffer - if we have space. */
          if (s_tcp_rx_tail.len == 0) {
            mbuf_free(&s_tcp_rx_tail);
            s_tcp_rx_tail.len = nc->recv_mbuf.len;
            s_tcp_rx_tail.buf = nc->recv_mbuf.buf;
            nc->recv_mbuf.buf = NULL;
            nc->recv_mbuf.len = 0;
            LOG(LL_DEBUG,
                ("Rescued %d bytes from closed conn", s_tcp_rx_tail.len));
          } else {
            LOG(LL_WARN,
                ("Dropped %d bytes on the floor", (int) nc->recv_mbuf.len));
          }
        }
        close_cur_conn();
      }
      if (nc == s_client_conn) {
        s_client_conn = NULL;
        s_last_connect_attempt = mg_time();
      }
      break;
    }
  }
}

static void tu_ws_conn_handler(struct mg_connection *nc, int ev, void *ev_data,
                               void *user_data) {
  mgos_wdt_feed();
  int uart_no = (s_ucfg == NULL) ? -1 : s_ucfg->uart_no;

  switch (ev) {
    case MG_EV_WEBSOCKET_FRAME: {
      struct websocket_message *wm = (struct websocket_message *) ev_data;
      size_t len = 0;
      LOG(LL_DEBUG, ("ws frame %d", (int) wm->size));
      if (uart_no >= 0) {
        /* Note: this write is blocking if wm->size exceeds write_avail. */
        mgos_uart_write(uart_no, wm->data, wm->size);
        s_last_activity = mg_time();
      } else {
        /* UART is disabled, throw away the frame. */
        len = wm->size;
      }
      if (len > 0) {
        LOG(LL_DEBUG, ("UART <- %d <- WS", (int) len));
      }
      if (len < wm->size) {
        /* UART buffer is full. Save the rest of the frame and throttle RX. */
        size_t tail_len = (wm->size - len);
        LOG(LL_DEBUG, ("%d bytes added to tail", (int) tail_len));
        mbuf_append(&s_tcp_rx_tail, wm->data + len, tail_len);
        nc->recv_mbuf_limit = 0;
      }
      break;
    }
    case MG_EV_SEND: {
      if (uart_no >= 0) {
        mgos_uart_schedule_dispatcher(uart_no, false /* from_isr */);
      }
      break;
    }
    case MG_EV_CLOSE: {
      LOG(LL_INFO, ("%p closed", nc));
      report_status(nc, 1 /* force */);
      if (s_tu_conn.type == TU_CONN_TYPE_TCP && nc == s_tu_conn.conn) {
        if (s_ucfg != NULL && s_ucfg->rx_throttle_when_no_net) {
          mgos_uart_set_rx_enabled(s_ucfg->uart_no, false);
        }
        close_cur_conn();
      }
      if (nc == s_client_conn) {
        s_client_conn = NULL;
        s_last_connect_attempt = mg_time();
      }
      break;
    }
  }
  (void) user_data;
}

static void tu_set_conn_tcp(struct mg_connection *nc, bool ws) {
  LOG(LL_INFO, ("New conn: %p%s", nc, (ws ? " (WS)" : "")));
  if (s_tu_conn.type != TU_CONN_TYPE_NONE) {
    if (mgos_sys_config_get_tu_evict_old()) {
      LOG(LL_INFO, ("Evicting %p", s_tu_conn.conn));
      close_cur_conn();
    } else {
      LOG(LL_INFO, ("Conn is already in place, dropping %p", nc));
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
      return;
    }
  }

  nc->handler = (ws ? tu_ws_conn_handler : tu_tcp_conn_handler);
#if CS_PLATFORM == CS_P_ESP8266
  mg_lwip_set_keepalive_params(nc, s_tcfg->keepalive.idle,
                               s_tcfg->keepalive.interval,
                               s_tcfg->keepalive.count);
#elif CS_PLATFORM == CS_P_CC3200
  /* On CC3200 keep-alive is enabled by default and can only be disabled. */
  SlSockKeepalive_t opt;
  opt.KeepaliveEnabled = s_tcfg->keepalive.enable;
  sl_SetSockOpt(nc->sock, SL_SOL_SOCKET, SL_SO_KEEPALIVE, (_u8 *) &opt,
                sizeof(opt));
#endif
  s_last_tcp_status_report = mg_time();
  if (s_tcfg->rx_buf_size > 0) nc->recv_mbuf_limit = s_tcfg->rx_buf_size;
  s_tu_conn.conn = nc;
  s_tu_conn.type = TU_CONN_TYPE_TCP;
  if (s_ucfg != NULL && s_ucfg->rx_throttle_when_no_net) {
    mgos_uart_set_rx_enabled(s_ucfg->uart_no, true);
  }
}

void tu_set_conn_bt_gatts(struct tu_bt_conn *tubc) {
  LOG(LL_INFO, ("New bt conn"));
  if (s_tu_conn.type != TU_CONN_TYPE_NONE) {
    if (mgos_sys_config_get_tu_evict_old()) {
      LOG(LL_INFO, ("Evicting %s", stringify_tu_conn(&s_tu_conn)));
      close_cur_conn();
    } else {
      LOG(LL_INFO, ("Conn is already in place, dropping new bt conn"));
      tu_bt_close(tubc);
      return;
    }
  }

  s_tu_conn.tubc = tubc;
  s_tu_conn.type = TU_CONN_TYPE_BT_GATTS;
  if (s_ucfg != NULL && s_ucfg->rx_throttle_when_no_net) {
    mgos_uart_set_rx_enabled(s_ucfg->uart_no, true);
  }
}

static void tu_conn_mgr(struct mg_connection *nc, int ev, void *ev_data,
                        void *user_data) {
  switch (ev) {
    case MG_EV_ACCEPT: {
      char addr[32];
      mg_sock_addr_to_str(&nc->sa, addr, sizeof(addr),
                          MG_SOCK_STRINGIFY_IP | MG_SOCK_STRINGIFY_PORT);
      LOG(LL_INFO, ("%p Connection from %s", nc, addr));
      if (s_tu_conn.type != TU_CONN_TYPE_NONE &&
          !mgos_sys_config_get_tu_evict_old()) {
        LOG(LL_INFO, ("The connection is already in place, dropping %p", nc));
        if (s_tcfg->listener.ws.enable) {
          mg_send_response_line(nc, 409, "Content-Type: text/plain\r\n");
          mg_printf(nc, "There is an existing connection\r\n");
          nc->flags |= MG_F_SEND_AND_CLOSE;
        } else {
          nc->flags |= MG_F_CLOSE_IMMEDIATELY;
        }
      } else {
        if (!s_tcfg->listener.ws.enable) {
          tu_set_conn_tcp(nc, false /* ws */);
        } else {
          /* Wait for handshake */
        }
      }
      break;
    }
    case MG_EV_WEBSOCKET_HANDSHAKE_REQUEST: {
      LOG(LL_INFO, ("%p WS handshake request", nc));
      break;
    }
    case MG_EV_WEBSOCKET_HANDSHAKE_DONE: {
      LOG(LL_INFO, ("%p WS handshake done", nc));
      tu_set_conn_tcp(nc, true /* ws */);
      break;
    }
    case MG_EV_CONNECT: {
      int res = *((int *) ev_data);
      LOG(LL_INFO, ("%p Connect result: %d", nc, res));
      if (res == 0) {
        if (s_tu_conn.type == TU_CONN_TYPE_NONE) {
          if (!s_tcfg->client.ws.enable) {
            tu_set_conn_tcp(nc, false /* ws */);
          } else {
            char *uri = strdup(s_tcfg->client.ws.uri);
            mgos_expand_mac_address_placeholders(uri);
            LOG(LL_INFO, ("%p Sending WS handshake to %s", nc, uri));
            mg_set_protocol_http_websocket(nc);
            mg_send_websocket_handshake2(nc, uri, s_tcfg->client.remote_addr,
                                         s_tcfg->client.ws.protocol, NULL);
            free(uri);
          }
        } else {
          /* We already have a connection (probably accepted one while
           * connecting), drop it. */
          LOG(LL_INFO, ("%p Already have %p, closing this one", nc,
                        stringify_tu_conn(&s_tu_conn)));
          nc->flags |= MG_F_CLOSE_IMMEDIATELY;
        }
      } else {
        /* Do nothing, wait for close event. */
      }
      break;
    }
    case MG_EV_CLOSE: {
      if (nc == s_client_conn) {
        LOG(LL_INFO, ("%p Closed", nc));
        s_client_conn = NULL;
        s_last_connect_attempt = mg_time();
      }
    }
  }

  (void) user_data;
}

static void tu_conn_mgr_timer_cb(void *arg) {
  check_beeper();
  /* TODO(dfrank): report status needs not only mongoose conn */
  report_status(s_tu_conn.conn, 0 /* force */);
  /* Initiate outgoing connection, if configured. */
  if (s_tu_conn.type == TU_CONN_TYPE_NONE && s_client_conn == NULL &&
      s_tcfg->client.remote_addr != NULL &&
      (mg_time() - s_last_connect_attempt) >=
          s_tcfg->client.reconnect_interval) {
    const char *error;
    struct mg_connect_opts copts;
    memset(&copts, 0, sizeof(copts));
    copts.ssl_cert = s_tcfg->client.tls.cert;
    copts.ssl_ca_cert = s_tcfg->client.tls.ca_cert;
    copts.ssl_server_name = s_tcfg->client.tls.server_name;
    copts.error_string = &error;
    LOG(LL_INFO,
        ("%p Connecting to %s (%s %s %s)", s_client_conn,
         s_tcfg->client.remote_addr, (copts.ssl_cert ? copts.ssl_cert : "-"),
         (copts.ssl_ca_cert ? copts.ssl_ca_cert : "-"),
         (copts.ssl_server_name ? copts.ssl_server_name : "-")));
    s_last_connect_attempt = mg_time();
    s_client_conn = mg_connect_opt(mgos_get_mgr(), s_tcfg->client.remote_addr,
                                   tu_conn_mgr, NULL, copts);
    if (s_client_conn == NULL) {
      LOG(LL_ERROR, ("Connection error: %s", error));
    }
  }
  (void) arg;
}

enum mgos_app_init_result tu_processor_init(void) __attribute__((weak));
enum mgos_app_init_result tu_processor_init(void) {
  return MGOS_APP_INIT_SUCCESS;
}

bool bt_rx_cb(struct mg_str value) {
  int len = MIN((int) value.len, mgos_sys_config_get_tu_bt_rx_buf_size());
  if (len != (int) value.len) {
    /*
     * We won't be able to handle all data, so, refuse to write it at all,
     * so that the client will be able to resend the request again and have
     * consistent data.
     */
    return false;
  }

  mbuf_append(&s_bt_buf_rx, value.p, len);
  tu_dispatch_to_uart(&s_bt_buf_rx, s_ucfg->uart_no);
  return true;
}

bool mgos_uart_bridge_init(void) {
  mbuf_init(&s_bt_buf_rx, 0);

  tu_bt_set_rx_cb(bt_rx_cb);
  tu_bt_set_conn_cb(tu_set_conn_bt_gatts);

  s_mcfg = mgos_sys_config_get_misc();
  s_last_activity = mg_time();
  LOG(LL_INFO, ("UART bridge init"));
  if (!init_tcp(mgos_sys_config_get_tcp())) return false;
  if (!init_uart(mgos_sys_config_get_uart())) return false;
  tu_bt_service_init();
  tu_uart_processor = tu_process_uart;
  return (tu_processor_init() == MGOS_APP_INIT_SUCCESS);
}
