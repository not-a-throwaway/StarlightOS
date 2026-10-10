#ifndef STARLIGHTOS_APP_MINESWEEPER_H
#define STARLIGHTOS_APP_MINESWEEPER_H

#include <stdint.h>

#define MINES_MAX_W 16
#define MINES_MAX_H 16

enum mines_difficulty {
    MINES_EASY = 0,
    MINES_CLASSIC = 1,
    MINES_EXPERT = 2
};

/* Standalone game model shared only with the user-mode Minesweeper
 * ELF. It has no kernel, window-system, or device dependencies. */
struct mines_game {
    uint8_t mines[MINES_MAX_W * MINES_MAX_H];
    uint8_t revealed[MINES_MAX_W * MINES_MAX_H];
    uint8_t flagged[MINES_MAX_W * MINES_MAX_H];
    int width, height, total;
    int started, over, won, difficulty;
    uint32_t seed;
};

void mines_game_new(struct mines_game* game, int difficulty, uint32_t entropy);
int mines_game_adjacent(const struct mines_game* game, int x, int y);
void mines_game_reveal(struct mines_game* game, int x, int y);
void mines_game_toggle_flag(struct mines_game* game, int x, int y);
int mines_game_index(int x, int y);

#endif
