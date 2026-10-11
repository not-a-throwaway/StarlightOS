#ifndef STARLIGHTOS_USER_SYSCALL_H
#define STARLIGHTOS_USER_SYSCALL_H
#include <stddef.h>
#include <stdint.h>
#include "slwindow.h"
#include "slnet.h"

static inline long starlightos_write(const char *buffer, size_t length)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(1), "D"(1), "S"(buffer), "d"(length) : "rcx", "r11", "memory");
    return result;
}

static inline int starlightos_readchar(void)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(2) : "rcx", "r11", "memory");
    return (int)result;
}

static inline void starlightos_exit(int status)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(60), "D"((uint64_t)status) : "rcx", "r11", "memory");
    for (;;) __asm__ volatile("pause");
}

/* ---- window syscalls (see slwindow.h) ----
 * Raw results, like the rest of this header: a negative return is -errno.
 */
static inline long starlightos_fbmap(int w, int h)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_FBMAP), "D"((uint64_t)w), "S"((uint64_t)h) : "rcx", "r11", "memory");
    return result;
}

static inline long starlightos_present(void)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_PRESENT) : "rcx", "r11", "memory");
    return result;
}

static inline long starlightos_getevents(struct sl_event *buf, int count, int timeout_ms)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_GETEVENTS), "D"((uint64_t)(uintptr_t)buf), "S"((uint64_t)count), "d"((uint64_t)(int64_t)timeout_ms) : "rcx", "r11", "memory");
    return result;
}

/* ---- network syscalls (see slnet.h) ----
 * Raw results, like the rest of this header: a negative return is -errno.
 */
static inline long starlightos_socket(int domain, int type, int protocol)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_SOCKET), "D"((uint64_t)domain), "S"((uint64_t)type), "d"((uint64_t)protocol) : "rcx", "r11", "memory");
    return result;
}

static inline long starlightos_connect(int fd, const struct sl_sockaddr_in *addr, int addrlen)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_CONNECT), "D"((uint64_t)fd), "S"((uint64_t)(uintptr_t)addr), "d"((uint64_t)addrlen) : "rcx", "r11", "memory");
    return result;
}

static inline long starlightos_send(int fd, const void *buf, size_t len)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_SEND), "D"((uint64_t)fd), "S"((uint64_t)(uintptr_t)buf), "d"((uint64_t)len) : "rcx", "r11", "memory");
    return result;
}

static inline long starlightos_recv(int fd, void *buf, size_t len)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_RECV), "D"((uint64_t)fd), "S"((uint64_t)(uintptr_t)buf), "d"((uint64_t)len) : "rcx", "r11", "memory");
    return result;
}

static inline long starlightos_poll(struct sl_pollfd *fds, int nfds, int timeout_ms)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_POLL), "D"((uint64_t)(uintptr_t)fds), "S"((uint64_t)nfds), "d"((uint64_t)(int64_t)timeout_ms) : "rcx", "r11", "memory");
    return result;
}

static inline long starlightos_resolve(const char *host, uint8_t out_ip[4])
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_RESOLVE), "D"((uint64_t)(uintptr_t)host), "S"((uint64_t)(uintptr_t)out_ip) : "rcx", "r11", "memory");
    return result;
}

static inline long starlightos_random(void *buf, size_t len)
{
    long result;
    __asm__ volatile("int $0x80" : "=a"(result) : "0"(SL_SYS_RANDOM), "D"((uint64_t)(uintptr_t)buf), "S"((uint64_t)len) : "rcx", "r11", "memory");
    return result;
}
#endif
