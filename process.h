#ifndef STARLIGHTOS_PROCESS_H
#define STARLIGHTOS_PROCESS_H

#include <stdint.h>

#if defined(__x86_64__)
void process_initialize(void);
int process_execute(const char *filename);
/* Runs an ELF64 app with a SysV-style argc/argv on its stack. Returns 0 if it ran (see
 * process_exit_status()), -1 if the file is missing/invalid. */
int process_execute_argv(const char *filename, int argc, char **argv);
int process_exit_status(void);     /* exit status of the most recent process (130 = Ctrl-C, 139 = fault) */
uint64_t process_syscall(void *saved_registers);
void process_handle_user_fault(uint64_t vector, uint64_t error, uint64_t rip);
extern volatile uint32_t process_exit_pending;
#endif

#endif
