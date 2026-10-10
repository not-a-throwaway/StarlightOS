#include <stdint.h>
#include "syscall.h"
#include "../apps/minesweeper.h"

static struct mines_game game;
static uint32_t random_seed = 0x6D79736Fu;

static size_t text_length(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s) { starlightos_write(s, text_length(s)); }

static void print_uint(unsigned value)
{
    char digits[10];
    unsigned n = 0;
    if (!value) { print("0"); return; }
    while (value && n < sizeof(digits)) {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (n) starlightos_write(&digits[--n], 1);
}

static void render(void)
{
    print("\n   ");
    for (int x = 0; x < game.width; x++) { print_uint((unsigned)(x % 10)); print(" "); }
    print("\n");
    for (int y = 0; y < game.height; y++) {
        print_uint((unsigned)y); print("  ");
        for (int x = 0; x < game.width; x++) {
            int i = mines_game_index(x, y);
            char c = '#';
            if (game.flagged[i]) c = 'F';
            else if (game.revealed[i]) {
                int near = mines_game_adjacent(&game, x, y);
                c = game.mines[i] ? '*' : near ? (char)('0' + near) : '.';
            } else if (game.over && game.mines[i]) c = '*';
            starlightos_write(&c, 1); print(" ");
        }
        print("\n");
    }
    if (game.won) print("You cleared the minefield!\n");
    else if (game.over) print("Boom! Game over.\n");
    else print("Commands: r x y reveal, f x y flag, n new, q quit\n");
}

static int read_line(char *out, int cap)
{
    int n = 0;
    while (n < cap - 1) {
        int c = starlightos_readchar();
        if (c == '\r') c = '\n';
        if (c == '\n') { out[n] = 0; print("\n"); return n; }
        if (c == '\b') { if (n) { n--; print("\b \b"); } continue; }
        if (c >= 32 && c < 127) { out[n++] = (char)c; starlightos_write((char *)&out[n - 1], 1); }
    }
    out[n] = 0;
    return n;
}

static int parse_int(const char **p, int *out)
{
    while (**p == ' ') (*p)++;
    if (**p < '0' || **p > '9') return -1;
    int n = 0;
    while (**p >= '0' && **p <= '9') n = n * 10 + (*(*p)++ - '0');
    *out = n;
    return 0;
}

static void new_game(void)
{
    random_seed = random_seed * 1664525u + 1013904223u;
    mines_game_new(&game, MINES_CLASSIC, random_seed);
}

void _start(void)
{
    new_game();
    print("StarlightOS Minesweeper (user mode)\nClassic 16x16, 40 mines. Coordinates are zero-based.\n");
    char line[32];
    for (;;) {
        render(); print("> ");
        read_line(line, sizeof(line));
        const char *p = line;
        char command = *p++;
        if (command == 'q' || command == 'Q') starlightos_exit(0);
        if (command == 'n' || command == 'N') { new_game(); continue; }
        if ((command == 'r' || command == 'R' || command == 'f' || command == 'F') && p[0] == ' ') {
            int x, y;
            if (parse_int(&p, &x) || parse_int(&p, &y)) { print("Use: r x y or f x y\n"); continue; }
            if (command == 'r' || command == 'R') mines_game_reveal(&game, x, y);
            else mines_game_toggle_flag(&game, x, y);
        } else print("Use: r x y, f x y, n, or q\n");
    }
}
