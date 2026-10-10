#ifndef SERIAL_H
#define SERIAL_H
/* COM1 (0x3F8) 115200 8N1 debug console. Safe on machines without a UART:
 * serial_init() runs a loopback test and serial_putc() becomes a no-op. */
void serial_init(void);
int  serial_present(void);
void serial_putc(char c);
void serial_write(const char *s);
int  serial_getc(void);   /* -1 when no byte is waiting */
#endif
