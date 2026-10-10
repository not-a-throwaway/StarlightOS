#ifndef KSCHED_H
#define KSCHED_H
#include <stdint.h>

/*
 * Cooperative "background work" hooks.
 *
 * StarlightOS has no preemptive scheduler. Long-running operations (network
 * waits, audio playback, shell commands) instead call kyield() whenever
 * they would otherwise spin. kyield() runs every registered poller:
 * the NIC receive path, the audio mixer, and the desktop redraw, so
 * the system stays alive while a command is busy.
 */
typedef void (*kpoll_fn)(void);

void ksched_register(kpoll_fn fn);
void kyield(void);                  /* run pollers once (never sleeps) */
void kidle(void);                   /* run pollers, then hlt until next interrupt */
void ksleep_ms(uint32_t ms);        /* yields while waiting; returns early on Ctrl-C */

/* Ctrl-C support. keyboard.c sets the flag, long commands poll it. */
void kabort_raise(void);
void kabort_clear(void);
int  kabort_requested(void);

/* True while a shell command is executing (GUI uses it to avoid re-entry). */
int  ksched_in_command(void);
void ksched_set_in_command(int v);

#endif
