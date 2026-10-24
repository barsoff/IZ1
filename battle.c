#include "battle.h"
#include "output.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int dist(int x, int y, const Tank *t) {
    return abs(x - t->x) + abs(y - t->y);
}

static int tank_at(const Battle *b, int x, int y) {
    for (int i = 0; i < b->n; ++i) {
        if (b->tank[i].x == x && b->tank[i].y == y) {
            return i;
        }
    }
    return -1;
}

static int free_cell(const Battle *b, int x, int y) {
    return x >= 0 && x < b->w && y >= 0 && y < b->h
        && b->cell[y][x] != '#' && tank_at(b, x, y) < 0;
}

/* каждый танк рассчитывает намерение по неизменному состоянию поля */
void tank_plan(const Battle *b, int index, Plan *plan) {
    static const int dx[] = {0, 1, 0, -1};
    static const int dy[] = {-1, 0, 1, 0};
    const Tank *t = &b->tank[index];
    *plan = (Plan) {.x = t->x, .y = t->y, .dst = -1};
    if (!t->hp) {
        return;
    }

    int near = -1;
    int best = MAX_W + MAX_H + 1;
    for (int j = 0; j < b->n; ++j) {
        const Tank *e = &b->tank[j];
        if (e->side == t->side || !e->hp) {
            continue;
        }
        int d = dist(t->x, t->y, e);
        if (near < 0 || d < best || (d == best && e->id < b->tank[near].id)) {
            near = j;
            best = d;
        }
    }
    if (near >= 0 && best <= t->range) {
        plan->dst = near;
    }

    /* Наступление: сближение до дальности стрельбы, без поиска обходных путей */
    if (near >= 0 && best > t->range && b->team[t->side].mode == ADVANCE) {
        for (int k = 0; k < 4; ++k) {
            int x = t->x + dx[k];
            int y = t->y + dy[k];
            int d = dist(x, y, &b->tank[near]);
            if (free_cell(b, x, y) && d < best) {
                plan->x = x;
                plan->y = y;
                best = d;
            }
        }
    }
}

static void fire(Battle *b, const Cfg *cfg, const Plan plan[]) {
    for (int i = 0; i < b->n; ++i) {
        const Tank *t = &b->tank[i];
        if (!t->hp || plan[i].dst < 0) {
            continue;
        }
        const Tank *dst = &b->tank[plan[i].dst];
        Shot s = {.src = i, .x = dst->x, .y = dst->y,
                  .hit = rand() % 100 < cfg->acc, .due = b->step + (unsigned) cfg->fly};
        b->shot[b->ns++] = s;
        ++b->team[t->side].fired;
        dprintf(STDOUT_FILENO, "Такт %llu: %c%d стреляет из (%d, %d) в (%d, %d); прилёт на такте %llu.\n",
                 b->step, 'A' + t->side, t->id, t->x, t->y, s.x, s.y, s.due);
    }
}

static void move(Battle *b, Plan plan[]) {
    /* Сначала находим все конфликты. Изменять планы при поиске нельзя */
    int conflicts = 0;
    for (int i = 0; i < b->n; ++i) {
        if (!b->tank[i].hp) {
            continue;
        }
        for (int j = i + 1; j < b->n; ++j) {
            if (b->tank[j].hp && plan[i].x == plan[j].x && plan[i].y == plan[j].y) {
                plan[i].blocked = plan[j].blocked = 1;
                const Tank *a = &b->tank[i], *c = &b->tank[j];
                dprintf(STDOUT_FILENO, "Такт %llu: конфликт %c%d и %c%d за (%d, %d); оба остаются на месте.\n",
                         b->step, 'A' + a->side, a->id, 'A' + c->side, c->id, plan[i].x, plan[i].y);
                ++conflicts;
            }
        }
    }
    if (!conflicts) {
        dprintf(STDOUT_FILENO, "Такт %llu: конфликтов клеток нет.\n", b->step);
    }
    for (int i = 0; i < b->n; ++i) {
        Tank *t = &b->tank[i];
        if (!t->hp || plan[i].blocked || (plan[i].x == t->x && plan[i].y == t->y)) {
            continue;
        }
        dprintf(STDOUT_FILENO, "Такт %llu: %c%d переместился из (%d, %d) в (%d, %d).\n",
                 b->step, 'A' + t->side, t->id, t->x, t->y, plan[i].x, plan[i].y);
        t->x = plan[i].x;
        t->y = plan[i].y;
    }
}

static void impact(Battle *b) {
    int dmg[MAX_TANKS] = {0};
    int left = 0;
    for (int i = 0; i < b->ns; ++i) {
        Shot s = b->shot[i];
        if (s.due > b->step) {
            b->shot[left++] = s;
            continue;
        }
        const Tank *src = &b->tank[s.src];
        int j = tank_at(b, s.x, s.y);
        if (!s.hit) {
            ++b->team[src->side].miss;
            dprintf(STDOUT_FILENO, "Такт %llu: снаряд %c%d промахнулся по клетке (%d, %d).\n",
                     b->step, 'A' + src->side, src->id, s.x, s.y);
        } else if (j < 0 || !b->tank[j].hp) {
            ++b->team[src->side].miss;
            dprintf(STDOUT_FILENO, "Такт %llu: снаряд %c%d достиг (%d, %d), действующего танка там нет.\n",
                     b->step, 'A' + src->side, src->id, s.x, s.y);
        } else {
            const Tank *dst = &b->tank[j];
            ++b->team[src->side].hit;
            ++dmg[j];
            dprintf(STDOUT_FILENO, "Такт %llu: снаряд %c%d попал в %c%d в (%d, %d).\n",
                     b->step, 'A' + src->side, src->id, 'A' + dst->side, dst->id, s.x, s.y);
        }
    }
    b->ns = left;
    /* Все попадания учтены по одному состоянию; теперь применяем сумму урона */
    for (int i = 0; i < b->n; ++i) {
        Tank *t = &b->tank[i];
        if (!dmg[i]) {
            continue;
        }
        int old = t->hp;
        t->hp = old > dmg[i] ? old - dmg[i] : 0;
        dprintf(STDOUT_FILENO, "Такт %llu: %c%d получил урон %d, прочность %d -> %d.\n",
                 b->step, 'A' + t->side, t->id, dmg[i], old, t->hp);
        if (!t->hp) {
            dprintf(STDOUT_FILENO, "Такт %llu: %c%d уничтожен, обломки остаются в (%d, %d).\n",
                     b->step, 'A' + t->side, t->id, t->x, t->y);
        }
    }
}

void battle_step(Battle *b, const Cfg *cfg, Plan plan[]) {
    dprintf(STDOUT_FILENO, "\n--- Такт %llu ---\n", b->step);
    out_plans(b, plan);
    fire(b, cfg, plan);
    move(b, plan);
    impact(b);
}

int alive(const Battle *b, int side) {
    int n = 0;
    for (int i = 0; i < b->n; ++i) {
        n += b->tank[i].side == side && b->tank[i].hp > 0;
    }
    return n;
}

End battle_end(const Battle *b, const Cfg *cfg) {
    int a = alive(b, 0), c = alive(b, 1);
    if (!a && !c) return DRAW;
    if (!c) return WIN_A;
    if (!a) return WIN_B;
    if (cfg->limit && b->step >= (unsigned) cfg->limit) return LIMIT;
    return RUN;
}

/* Проверяем основные инварианты после каждого полного такта */
int battle_valid(const Battle *b) {
    if (b->ns < 0 || b->ns > MAX_SHOTS) return 0;
    for (int i = 0; i < b->n; ++i) {
        const Tank *t = &b->tank[i];
        if (t->hp < 0 || t->x < 0 || t->x >= b->w || t->y < 0 || t->y >= b->h
            || b->cell[t->y][t->x] == '#') return 0;
        for (int j = i + 1; j < b->n; ++j) {
            if (t->x == b->tank[j].x && t->y == b->tank[j].y) return 0;
        }
    }
    return 1;
}
