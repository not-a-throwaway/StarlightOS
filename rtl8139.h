#ifndef RTL8139_H
#define RTL8139_H
#include <stdint.h>

struct rtl8139_stats {
    uint32_t rx_frames, rx_bytes, rx_errors, rx_overflows;
    uint32_t tx_frames, tx_bytes, tx_errors, tx_timeouts;
};

int  rtl8139_initialize(void);
int  rtl8139_ready(void);
int  rtl8139_link_up(void);
int  rtl8139_send(const uint8_t *frame, uint16_t length);
int  rtl8139_receive(uint8_t *frame, uint16_t capacity, uint16_t *length);
const uint8_t *rtl8139_mac_address(void);
const struct rtl8139_stats *rtl8139_get_stats(void);
int  rtl8139_irq_line(void);
#endif
