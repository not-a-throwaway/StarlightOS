#ifndef NET_H
#define NET_H
/*
 * StarlightOS network stack.
 *
 *   rtl8139  ->  Ethernet / ARP  ->  IPv4 / ICMP  ->  UDP / TCP
 *                                                      |     |
 *                                  DHCP, DNS, NTP <----+     +--> HTTP/1.1
 *
 * Everything is polled: net_poll() drains the NIC and runs timers. It is
 * called from kyield() (so it runs while any command is waiting, and from
 * the desktop loop when idle), which means no network IRQ is needed.
 */
#include <stdint.h>
#include <stddef.h>

struct net_config {
    uint8_t  mac[6];
    uint8_t  ip[4], mask[4], gateway[4];
    uint8_t  dns[2][4];
    int      dns_count;
    uint8_t  ntp[4];
    int      has_ntp;
    uint8_t  dhcp_server[4];
    uint32_t lease_secs;
    uint32_t lease_start_ms;
    char     domain[48];
    char     hostname[16];
    int      configured;        /* has a usable IPv4 address */
};

struct net_stats {
    uint32_t arp_rx, arp_tx, ip_rx, ip_tx, icmp_rx, icmp_tx;
    uint32_t udp_rx, udp_tx, tcp_rx, tcp_tx, tcp_retrans, tcp_resets;
    uint32_t tcp_ws;                         /* connections that negotiated window scaling */
    uint32_t drop_csum, drop_other;
};

/* ---- interface ---- */
int  net_initialize(void);                    /* probes the NIC and starts DHCP in the background */
int  net_ready(void);                         /* NIC present */
int  net_link_up(void);
int  net_configured(void);                    /* has an IPv4 address */
int  net_wait_configured(uint32_t timeout_ms);/* yields while waiting (Ctrl-C aware) */
void net_poll(void);
const struct net_config *net_get_config(void);
const struct net_stats  *net_get_stats(void);
int  net_dhcp_restart(void);                  /* drop lease and request a new one */
void net_set_static(const uint8_t ip[4], const uint8_t mask[4], const uint8_t gw[4], const uint8_t dns[4]);
void net_set_dns(const uint8_t dns[4]);
void net_set_hostname(const char *name);

int  net_parse_ip(const char *s, uint8_t ip[4]);
void net_fmt_ip(char *out, const uint8_t ip[4]);     /* out >= 16 bytes */
void net_fmt_mac(char *out, const uint8_t mac[6]);   /* out >= 18 bytes */

/* ---- ARP ---- */
struct arp_info { uint8_t ip[4], mac[6]; uint32_t age_ms; };
int  net_arp_table(struct arp_info *out, int max);
int  net_arp_resolve(const uint8_t ip[4], uint8_t mac[6], uint32_t timeout_ms);

/* ---- ICMP ---- */
enum { PING_REPLY = 0, PING_TIMEOUT = 1, PING_TTL_EXCEEDED = 2, PING_UNREACHABLE = 3, PING_ERROR = 4 };
struct ping_result {
    int      status;
    uint8_t  from[4];
    uint32_t rtt_us;
    uint8_t  ttl;       /* TTL of the reply packet */
    uint16_t bytes;     /* ICMP payload bytes received */
};
int net_ping(const uint8_t dst[4], uint16_t seq, uint8_t ttl, uint16_t payload,
             uint32_t timeout_ms, struct ping_result *res);

/* ---- UDP ---- */
int  udp_open(uint16_t port);                  /* port 0 = ephemeral; returns handle or -1 */
void udp_close(int h);
int  udp_sendto(int h, const uint8_t ip[4], uint16_t port, const void *data, uint16_t len);
int  udp_recvfrom(int h, void *buf, uint16_t cap, uint8_t from_ip[4], uint16_t *from_port, uint32_t timeout_ms);

/* ---- TCP ---- */
enum tcp_state {
    TCP_CLOSED = 0, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED,
    TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT
};
enum { TCP_ERR_TIMEOUT = -1, TCP_ERR_RESET = -2, TCP_ERR_REFUSED = -3, TCP_ERR_NOSOCK = -4,
       TCP_ERR_ABORT = -5, TCP_ERR_NOROUTE = -6 };

int  tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms);   /* socket or TCP_ERR_* */
int  tcp_listen(uint16_t port);                                              /* socket or TCP_ERR_* */
int  tcp_accept(int listener, uint32_t timeout_ms);                          /* socket, or -1 on timeout */
int  tcp_send(int s, const void *data, size_t len);                          /* bytes accepted, or TCP_ERR_* */
int  tcp_recv(int s, void *buf, size_t cap, uint32_t timeout_ms);            /* >0 data, 0 EOF, -1 timeout, <-1 error */
int  tcp_available(int s);                                                   /* bytes ready to read */
void tcp_close(int s);                                                       /* graceful close + release */
void tcp_close_nowait(int s);                                                /* queue the FIN and return at once (event loops) */
void tcp_abort(int s);                                                       /* RST + release */
int  tcp_get_state(int s);

/* poll integration (poll.h): readiness bits, wait queue, free send-buffer bytes */
struct kwq;
unsigned tcp_poll_mask(int s);                                               /* POLLIN|POLLOUT|POLLHUP|POLLERR */
struct kwq *tcp_waitq(int s);
int  tcp_tx_space(int s);
int  tcp_peer(int s, uint8_t ip[4], uint16_t *port);

struct tcp_info { int sock; int state; uint8_t remote_ip[4]; uint16_t local_port, remote_port; uint32_t rx_queued, tx_queued;
                  uint32_t snd_wnd; uint8_t snd_wscale, rcv_shift, ws_ok; };
int  tcp_list(struct tcp_info *out, int max);
const char *tcp_state_name(int state);

/* ---- DNS ---- */
#define DNS_MAX_ADDRS 4
struct dns_result {
    uint8_t  addrs[DNS_MAX_ADDRS][4];
    int      addr_count;
    char     cname[64];            /* last CNAME seen, "" if none */
    uint32_t ttl;
    uint32_t rtt_us;
    int      rcode;                /* DNS RCODE of the reply, -1 on timeout */
    uint8_t  server[4];
};
int  dns_query(const char *name, struct dns_result *res);     /* 0 if at least one A record */
int  dns_query_server(const uint8_t server[4], uint16_t port, const char *name, struct dns_result *res);
int  dns_resolve(const char *name_or_ip, uint8_t ip[4]);      /* literal IPs, cache, then query */
void dns_cache_flush(void);
int  dns_cache_list(char names[][48], uint8_t ips[][4], uint32_t *ttl_left, int max);

/* ---- NTP ---- */
struct ntp_result { uint32_t unix_secs; int32_t offset_ms; uint32_t rtt_ms; uint8_t stratum; uint8_t server_ip[4]; };
int  ntp_query(const char *server, struct ntp_result *res);

/* ---- HTTP / HTTPS (http.c; https:// uses the TLS 1.3 client in tls.c) ---- */
struct http_opts {
    const char *method;            /* NULL = GET */
    const char *body;              /* POST body, may be NULL */
    size_t      body_len;
    const char *extra_headers;     /* "Name: value\r\n..." */
    int         follow_redirects;  /* max hops, 0 = don't follow */
    uint32_t    timeout_ms;        /* per-read timeout */
    size_t      max_body;          /* 0 = default (4 MiB) */
    int         verbose;           /* print request/response head lines */
    int         quiet;             /* suppress progress output */
    void      (*progress)(size_t received, int total);   /* optional */
};
struct http_response {
    int      status;               /* 0 if no valid response */
    char     status_text[40];
    char     content_type[64];
    char     location[192];
    char     final_url[256];
    char     headers[1536];        /* raw header block of the final response */
    uint8_t *body;                 /* kmalloc'd, NUL-terminated; free with kfree */
    size_t   body_len;
    int      content_length;       /* -1 if unknown */
    int      truncated;
    uint32_t elapsed_ms;
    int      redirects;
    int      tls_error;            /* TLS_E_* when error == -7 (TLS handshake failed), else 0 */
    int      tls_alert;            /* server alert description byte, -1 if none */
    int      error;                /* 0 ok, else negative code (see http_error_text) */
};
int  http_request(const char *url, const struct http_opts *opts, struct http_response *res);
const char *http_error_text(int code);
void http_response_free(struct http_response *res);

/* ---- HTML -> text (html.c) ---- */
/* Renders HTML to plain wrapped text. Returns length written (NUL-terminated). */
size_t html_to_text(const char *html, size_t len, char *out, size_t cap, int width, int show_links);

/* ---- URL helpers ---- */
struct url_parts { char host[96]; uint16_t port; char path[200]; int https; };
int  url_parse(const char *url, struct url_parts *out);

#endif
