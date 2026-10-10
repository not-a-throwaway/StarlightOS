# poll() / select() for StarlightOS

Files: `poll.h poll.c` (core), hooks in `tcp.c keyboard.c net.h`, consumer `httpd.c`,
tests `tests/host/poll_host.c poll_test.py` (`make polltest`).

## Model
StarlightOS is cooperative: there is one flow of control and `kyield()` runs the background
pollers (NIC, audio, desktop). "Blocking" therefore means *parking on wait queues and halting
the CPU between events*, not context switching.

    producer (tcp_input / tcp_timers / keyboard IRQ)
        -> kwq_wake_all(&queue)            sets waiter->woken = 1, nothing else (IRQ-safe)
    consumer kpoll():
        w.woken = 0
        for each fd: kwq_add(fd.waitq, entry, &w)   <- register BEFORE reading state
                     mask = fd.ops->poll(fd)
        ready?  -> return
        else    -> net_poll(); kyield();             (this is where packets get processed,
                   cli; if (!w.woken) sti; hlt        which calls kwq_wake_all)
                   re-scan, re-check timeout / Ctrl-C
        finally unregister every entry

Why no wake-up is lost: entries are added before state is read, `woken` is cleared before each
scan, and the flag test + `sti; hlt` run with IF cleared (`sti` takes effect after the next
instruction).

Latency: the NIC is polled (no RX IRQ), so `hlt` ends at the next interrupt, the 100 Hz PIT tick:
at most ~10 ms from packet arrival to wake-up, ~0% CPU idle. Wiring the RTL8139 IRQ to
`kwq_wake_all` would make it instant; not needed for correctness.

## API
    struct pollfd { int fd; short events, revents; };      POLLIN POLLOUT POLLERR POLLHUP POLLNVAL
    int kpoll(struct pollfd *fds, unsigned n, int timeout_ms);   // <0 forever, 0 check, >0 ms
    int kselect(int nfds, kfd_set *rd, kfd_set *wr, kfd_set *ex, int timeout_ms);
    int kfd_open_tcp / kfd_open_listener / kfd_open_tty / kfd_close
    int kfd_read / kfd_write / kfd_accept        // never block: KE_AGAIN instead

Descriptors (`kfd`) are a vtable `{poll, waitq, read, write, close}`, so a new pollable object
(UDP socket, pipe, window event queue) is one `struct kfd_ops` plus a producer that calls
`kwq_wake_all`.

## Hook points
* `tcp.c`: `tcp_input()`/`tcp_timers()` are now thin wrappers that run the old body and then wake
  every socket queue that has waiters (and a listener whose child completed the handshake).
  `tcp_poll_mask()`, `tcp_waitq()`, `tcp_tx_space()`, `tcp_close_nowait()` are new.
  `TCP_SOCKS` 8 -> 16.
* `keyboard.c`: `queue_character()` wakes the tty queue; `keyboard_has_char()` added.
* `kernel.c`: `kfd_init()` before `net_initialize()`.

## Limits / not done
* No non-blocking `connect` (`tcp_connect` still waits), no UDP fd type yet.
* `kpoll` is not exposed as a ring-3 syscall: processes now have a typed descriptor table
  (files today, sockets/event queues when their syscalls land) but no `poll()` entry point yet.
* Not boot-tested on QEMU/hardware (no cross toolchain was available): the poll core and httpd were
  run on the host against a mock TCP layer (`make polltest`, under ASan+UBSan).
