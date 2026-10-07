#include "output.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
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

void out_field(const Battle *b) {
    char cell[MAX_H][MAX_W];
    memcpy(cell, b->cell, sizeof(cell));
    for (int i = 0; i < b->n; ++i) {
        const Tank *t = &b->tank[i];
        cell[t->y][t->x] = t->hp ? (char) ('A' + t->side) : 'X';
    }
    dprintf(STDOUT_FILENO, "Поле после такта %llu:\n    ", b->step);
    for (int x = 0; x < b->w; ++x) {
        dprintf(STDOUT_FILENO, "%2d ", x);
    }
    dprintf(STDOUT_FILENO, "\n");
    for (int y = 0; y < b->h; ++y) {
        dprintf(STDOUT_FILENO, "%2d  ", y);
        for (int x = 0; x < b->w; ++x) {
            dprintf(STDOUT_FILENO, " %c ", cell[y][x]);
        }
        dprintf(STDOUT_FILENO, "\n");
    }
    for (int i = 0; i < b->n; ++i) {
        const Tank *t = &b->tank[i];
        dprintf(STDOUT_FILENO, "  %c%d: (%d, %d), прочность %d, дальность %d%s\n",
                 'A' + t->side, t->id, t->x, t->y, t->hp, t->range, t->hp ? "" : ", уничтожен");
    }
}

/* Один и тот же итог выводим на экран и в файл через переданный дескриптор */
int out_stats(int fd, const Battle *b, End end) {
    const char *msg = "Ошибка выполнения";
    switch (end) {
        case WIN_A: msg = "Победа стороны A"; break;
        case WIN_B: msg = "Победа стороны B"; break;
        case DRAW: msg = "Ничья: обе стороны уничтожены или отсутствуют"; break;
        case LIMIT: msg = "Достигнут лимит тактов"; break;
        case STOP: msg = "Прервано пользователем"; break;
        default: break;
    }
    int ok = dprintf(fd, "\n=== Итоги ===\nПричина: %s\nТактов: %llu\n", msg, b->step) >= 0;
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
