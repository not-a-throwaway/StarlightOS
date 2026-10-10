/* CDEMO.ELF -- a normal hosted C program: newlib's stdio/malloc/string/math on StarlightOS.
 *
 *   CDEMO.ELF                 self-test of the libc port
 *   CDEMO.ELF a b c           shows argc/argv
 *   CDEMO.ELF -i              interactive: echoes lines until EOF (Ctrl-D) -- stdin is cooked
 *   CDEMO.ELF -x 7            exits with status 7 (check with `echo $?`)
 *   CDEMO.ELF -cat FILE       like cat, through open()/read()
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int selftest(void)
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("  FAIL: %s\n", #c); fails++; } else printf("  ok:   %s\n", #c); } while (0)

    char buf[64];
    snprintf(buf, sizeof buf, "%d|%5.2f|%s|%x", 42, 3.14159, "str", 255);
    CHECK(strcmp(buf, "42| 3.14|str|ff") == 0);

    int *a = malloc(1000 * sizeof(int));
    CHECK(a != NULL);
    for (int i = 0; i < 1000; i++) a[i] = (i * 7919) % 1000;
    qsort(a, 1000, sizeof(int), cmp_int);
    int sorted = 1;
    for (int i = 1; i < 1000; i++) if (a[i - 1] > a[i]) sorted = 0;
    CHECK(sorted);
    free(a);

    void *big = malloc(512 * 1024);                    /* exercises _sbrk() */
    CHECK(big != NULL);
    if (big) { memset(big, 0xAB, 512 * 1024); free(big); }

    CHECK(fabs(sqrt(2.0) - 1.41421356) < 1e-6);
    CHECK(strtol("-1234", NULL, 10) == -1234);

    FILE *f = fopen("CDEMO.OUT", "w");
    CHECK(f != NULL);
    if (f) {
        fprintf(f, "line one\nline two %d\n", 2);
        CHECK(fclose(f) == 0);
        f = fopen("CDEMO.OUT", "r");
        CHECK(f != NULL);
        if (f) {
            char l1[64] = "", l2[64] = "";
            fgets(l1, sizeof l1, f); fgets(l2, sizeof l2, f);
            CHECK(strcmp(l1, "line one\n") == 0 && strcmp(l2, "line two 2\n") == 0);
            fclose(f);
        }
        CHECK(remove("CDEMO.OUT") == 0);
        CHECK(fopen("CDEMO.OUT", "r") == NULL && errno == ENOENT);
    }
    time_t t = time(NULL);
    CHECK(t > 1600000000);
    return fails;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "-x")) return atoi(argv[2]);

    if (argc >= 3 && !strcmp(argv[1], "-cat")) {
        FILE *f = fopen(argv[2], "r");
        if (!f) { perror(argv[2]); return 1; }
        int c;
        while ((c = fgetc(f)) != EOF) putchar(c);
        fclose(f);
        return 0;
    }

    if (argc == 2 && !strcmp(argv[1], "-i")) {
        char line[128];
        printf("type lines, Ctrl-D to finish\n");
        while (printf("> "), fflush(stdout), fgets(line, sizeof line, stdin)) printf("you said: %s", line);
        printf("\nbye\n");
        return 0;
    }

    printf("Hello from newlib on StarlightOS!\n");
    printf("argc = %d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    if (argc > 1) return 0;

    printf("self-test:\n");
    int fails = selftest();
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "all passed", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
