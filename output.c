#include "output.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int out_open(const char *path, const char *src) {
    int fd = open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть журнал: %s\n", strerror(errno));
        return -1;
    }
    /* Проверяем файл до усечения, чтобы --log не сломал входной сценарий */
    struct stat log_st, src_st;
    if (fstat(fd, &log_st) < 0 || !S_ISREG(log_st.st_mode)) {
        dprintf(STDERR_FILENO, "Журнал должен быть обычным файлом.\n");
        close(fd);
        return -1;
    }
    if (src && stat(src, &src_st) == 0
        && log_st.st_dev == src_st.st_dev && log_st.st_ino == src_st.st_ino) {
        dprintf(STDERR_FILENO, "Сценарий и журнал не могут быть одним файлом.\n");
        close(fd);
        return -1;
    }
    if (ftruncate(fd, 0) < 0) {
        dprintf(STDERR_FILENO, "Не удалось очистить журнал: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* выводим намерения в порядке танков до изменения поля */
void out_plans_fd(int fd, const Battle *b, const Plan plan[], int side) {
    for (int i = 0; i < b->n; ++i) {
        const Tank *t = &b->tank[i];
        if (!t->hp || (side >= 0 && t->side != side)) {
            continue;
        }
        int seen = 0;
        for (int j = 0; j < b->n; ++j) {
            const Tank *e = &b->tank[j];
            if (e->side == t->side || !e->hp) {
                continue;
            }
            int d = abs(t->x - e->x) + abs(t->y - e->y);
            if (d <= t->range) {
                ++seen;
                dprintf(fd, "Такт %llu: %c%d наблюдает %c%d, расстояние %d.\n",
                         b->step, 'A' + t->side, t->id, 'A' + e->side, e->id, d);
            }
        }
        if (!seen) {
            dprintf(fd, "Такт %llu: %c%d не наблюдает целей в дальности стрельбы.\n",
                     b->step, 'A' + t->side, t->id);
        }
        const Plan *p = &plan[i];
        if (p->dst >= 0) {
            const Tank *e = &b->tank[p->dst];
            dprintf(fd, "Такт %llu: %c%d выбирает цель %c%d, клетку (%d, %d).\n",
                     b->step, 'A' + t->side, t->id, 'A' + e->side, e->id, e->x, e->y);
        }
        if (p->x == t->x && p->y == t->y) {
            dprintf(fd, "Такт %llu: %c%d намерен остаться в (%d, %d).\n",
                     b->step, 'A' + t->side, t->id, t->x, t->y);
        } else {
            dprintf(fd, "Такт %llu: %c%d намерен перейти из (%d, %d) в (%d, %d).\n",
                     b->step, 'A' + t->side, t->id, t->x, t->y, p->x, p->y);
        }
    }
}

void out_field_fd(int fd, const Battle *b) {
    char cell[MAX_H][MAX_W];
    memcpy(cell, b->cell, sizeof(cell));
    for (int i = 0; i < b->n; ++i) {
        const Tank *t = &b->tank[i];
        cell[t->y][t->x] = t->hp ? (char) ('A' + t->side) : 'X';
    }
    dprintf(fd, "Поле после такта %llu:\n    ", b->step);
    for (int x = 0; x < b->w; ++x) {
        dprintf(fd, "%2d ", x);
    }
    dprintf(fd, "\n");
    for (int y = 0; y < b->h; ++y) {
        dprintf(fd, "%2d  ", y);
        for (int x = 0; x < b->w; ++x) {
            dprintf(fd, " %c ", cell[y][x]);
        }
        dprintf(fd, "\n");
    }
    for (int i = 0; i < b->n; ++i) {
        const Tank *t = &b->tank[i];
        dprintf(fd, "  %c%d: (%d, %d), прочность %d, дальность %d%s\n",
                 'A' + t->side, t->id, t->x, t->y, t->hp, t->range, t->hp ? "" : ", уничтожен");
    }
}

const char *out_end(End end) {
    switch (end) {
        case WIN_A: return "Победа стороны A";
        case WIN_B: return "Победа стороны B";
        case DRAW: return "Ничья: обе стороны уничтожены или отсутствуют";
        case LIMIT: return "Достигнут лимит тактов";
        case STOP: return "Прервано пользователем";
        default: return "Ошибка выполнения";
    }
}

/* Один и тот же итог выводим на экран и в файл через переданный дескриптор */
int out_stats(int fd, const Battle *b, End end) {
    int ok = dprintf(fd, "\n=== Итоги ===\nПричина: %s\nТактов: %llu\n", out_end(end), b->step) >= 0;
    for (int side = 0; side < 2; ++side) {
        int total = 0, pending = 0;
        for (int i = 0; i < b->n; ++i) total += b->tank[i].side == side;
        for (int i = 0; i < b->ns; ++i) pending += b->tank[b->shot[i].src].side == side;
        const Team *t = &b->team[side];
        if (dprintf(fd, "Сторона %c: осталось %d из %d, потери %d, выстрелы %llu, попадания %llu, без урона %llu, в полёте %d.\n",
                    'A' + side, alive(b, side), total, total - alive(b, side), t->fired, t->hit, t->miss, pending) < 0) {
            ok = 0;
        }
    }
    if (dprintf(fd, "Снарядов в полёте: %d. После завершения боя они не обрабатываются.\n", b->ns) < 0) {
        ok = 0;
    }
    return ok;
}

void out_field(const Battle *b) {
    out_field_fd(STDOUT_FILENO, b);
}
