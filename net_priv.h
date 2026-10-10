#ifndef NET_PRIV_H
#define NET_PRIV_H
#include "net.h"

static inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static inline void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline int ip_eq(const uint8_t *a, const uint8_t *b) { return a[0]==b[0] && a[1]==b[1] && a[2]==b[2] && a[3]==b[3]; }

uint16_t net_checksum(const void *data, size_t len, uint32_t initial);
uint32_t net_pseudo_sum(const uint8_t src[4], const uint8_t dst[4], uint8_t proto, uint16_t len);

/* Send an IP datagram. dst_mac may be NULL (resolved via ARP / gateway). */
int  ip_send_ex(const uint8_t src[4], const uint8_t dst[4], const uint8_t *dst_mac,
                uint8_t proto, uint8_t ttl, const uint8_t *payload, uint16_t len);
int  ip_send(const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint16_t len);
int  net_on_link(const uint8_t ip[4]);
int  net_in_poll(void);
struct net_stats *net_stats_rw(void);
struct net_config *net_config_rw(void);

void tcp_input(const uint8_t *src_ip, const uint8_t *dst_ip, const uint8_t *seg, uint16_t len);
void tcp_timers(void);
void dhcp_poll(void);
#endif
