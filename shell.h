#ifndef SHELL_H
#define SHELL_H
#include <stddef.h>
#include <stdint.h>

struct command {
    const char *name;
    const char *group;      /* "System", "Files", "Network", "Sound", "Text", "Shell" */
    const char *usage;
    const char *help;
    int (*fn)(int argc, char **argv);
};

void shell_initialize(void);
void shell_handle_character(char character);

/* Executes one command line (supports ; && || | > >> < 2> 2>&1, quoting, $VAR $(cmd) globs and if/while/for one-liners). Returns the exit status. */
int  shell_run_line(const char *line);
/* Runs a script file line by line. Returns 0, or -1 if the file can't be read. */
int  shell_run_script(const char *filename);
/* Same, with positional parameters: argv[0] is $0 (the script), argv[1..] are $1.. ; *status gets the script's
 * exit status (last command, or the `exit N` value). Returns 0, or -1 if the file can't be read. */
int  shell_run_script_args(const char *filename, int argc, char **argv, int *status);
/* Standard error: where err()/errmsg() text goes (the screen, a 2> file, or stdout for 2>&1). */
void shell_stderr_write(const char *text);

/* Standard input for the command that is running (output of the previous pipeline stage). */
extern const char *shell_stdin;
extern size_t      shell_stdin_len;

/* Blocking single-key read for interactive commands. timeout_ms 0 = forever.
 * Returns the key, or -1 on timeout / Ctrl-C. */
int  shell_getc(uint32_t timeout_ms);

/* Registers an extra command table (used by the net/sound/etc. modules). */
void shell_register(const struct command *table, int count);

/* Variables and aliases. */
const char *shell_getenv(const char *name);
void        shell_setenv(const char *name, const char *value);

/* Calls cb(name, value, user) for every set variable (used by `env`/`export`). */
void        shell_env_each(void (*cb)(const char *name, const char *value, void *user),
                           void *user);

/* Loads whole file into kmalloc'd, NUL-terminated memory. Caller kfree()s. */
char *shell_read_file(const char *name, size_t *len_out);

extern volatile int gui_requested;
#endif
