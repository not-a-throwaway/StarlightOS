#include "ksched.h"
#include "ktime.h"

#define MAX_POLLERS 8
static kpoll_fn pollers[MAX_POLLERS];
static int poller_count;
static volatile int in_yield;
static volatile int abort_flag;
static volatile int in_command;

void ksched_register(kpoll_fn fn)
{
    for (int i = 0; i < poller_count; i++) if (pollers[i] == fn) return;
    if (poller_count < MAX_POLLERS) pollers[poller_count++] = fn;
}

void kyield(void)
{
    if (in_yield) return;
    in_yield = 1;
    for (int i = 0; i < poller_count; i++) pollers[i]();
    in_yield = 0;
}

void kidle(void)
{
    kyield();
    __asm__ volatile("sti; hlt" ::: "memory");
}

void ksleep_ms(uint32_t ms)
{
    uint32_t start = ktime_ms();
    while ((uint32_t)(ktime_ms() - start) < ms && !abort_flag) kidle();
}

void kabort_raise(void) { abort_flag = 1; }
void kabort_clear(void) { abort_flag = 0; }
int  kabort_requested(void) { return abort_flag; }
int  ksched_in_command(void) { return in_command; }
void ksched_set_in_command(int v) { in_command = v; }
