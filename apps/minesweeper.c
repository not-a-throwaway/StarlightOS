#include "minesweeper.h"

int mines_game_index(int x, int y)
{
    return y * MINES_MAX_W + x;
}

void mines_game_new(struct mines_game* g, int difficulty, uint32_t entropy)
{
    if (difficulty < MINES_EASY || difficulty > MINES_EXPERT)
        difficulty = MINES_CLASSIC;
    g->difficulty = difficulty;
    g->width = difficulty == MINES_EASY ? 9 : MINES_MAX_W;
    g->height = difficulty == MINES_EASY ? 9 : MINES_MAX_H;
    g->total = difficulty == MINES_EASY ? 10 : difficulty == MINES_CLASSIC ? 40 : 60;
    for (int i = 0; i < MINES_MAX_W * MINES_MAX_H; i++)
        g->mines[i] = g->revealed[i] = g->flagged[i] = 0;
    g->seed ^= entropy + (uint32_t)(uintptr_t)g;
    if (!g->seed) g->seed = 0x9E3779B9u;
    int placed = 0;
    while (placed < g->total) {
        g->seed = g->seed * 1664525u + 1013904223u;
        int cell = (int)(g->seed % (uint32_t)(g->width * g->height));
        if (!g->mines[cell]) { g->mines[cell] = 1; placed++; }
    }
    g->started = g->over = g->won = 0;
}

int mines_game_adjacent(const struct mines_game* g, int x, int y)
{
    int count = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int nx = x + dx, ny = y + dy;
        if ((dx || dy) && nx >= 0 && ny >= 0 && nx < g->width && ny < g->height &&
            g->mines[mines_game_index(nx, ny)]) count++;
    }
    return count;
}

void mines_game_reveal(struct mines_game* g, int x, int y)
{
    if (x < 0 || y < 0 || x >= g->width || y >= g->height || g->over) return;
    int start = mines_game_index(x, y);
    if (g->flagged[start] || g->revealed[start]) return;
    if (!g->started) {
        g->started = 1;
        if (g->mines[start]) {
            g->mines[start] = 0;
            for (int i = 0; i < g->width * g->height; i++)
                if (!g->mines[i] && i != start) { g->mines[i] = 1; break; }
        }
    }
    int queue[MINES_MAX_W * MINES_MAX_H], head = 0, tail = 0;
    uint8_t queued[MINES_MAX_W * MINES_MAX_H] = {0};
    queue[tail++] = start;
    queued[start] = 1;
    while (head < tail) {
        int cell = queue[head++], cx = cell % MINES_MAX_W, cy = cell / MINES_MAX_W;
        if (g->revealed[cell] || g->flagged[cell]) continue;
        g->revealed[cell] = 1;
        if (g->mines[cell]) { g->over = 1; g->won = 0; break; }
        if (mines_game_adjacent(g, cx, cy)) continue;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = cx + dx, ny = cy + dy;
            if (nx >= 0 && ny >= 0 && nx < g->width && ny < g->height) {
                int next = mines_game_index(nx, ny);
                if (!queued[next] && !g->revealed[next] && !g->flagged[next] && tail < MINES_MAX_W * MINES_MAX_H) {
                    queue[tail++] = next;
                    queued[next] = 1;
                }
            }
        }
    }
    if (!g->over) {
        int safe = 0;
        for (int i = 0; i < g->width * g->height; i++) if (!g->mines[i] && g->revealed[i]) safe++;
        if (safe == g->width * g->height - g->total) g->over = g->won = 1;
    }
}

void mines_game_toggle_flag(struct mines_game* g, int x, int y)
{
    if (!g->over && x >= 0 && y >= 0 && x < g->width && y < g->height) {
        int cell = mines_game_index(x, y);
        if (!g->revealed[cell]) g->flagged[cell] ^= 1;
    }
}
