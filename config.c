#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int num(const char *s, int lo, int hi, int *val) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < lo || v > hi) {
        return 0;
    }
    *val = (int) v;
    return 1;
}

void cfg_default(Cfg *cfg, Battle *b) {
    *cfg = (Cfg) {.limit = 100, .acc = 80, .fly = 1, .seed = 24, .delay = 200};
    *b = (Battle) {.w = 12, .h = 8, .n = 4};
    for (int y = 0; y < b->h; ++y) {
        for (int x = 0; x < b->w; ++x) {
            b->cell[y][x] = '.';
        }
    }
    b->cell[2][5] = b->cell[3][5] = '#';
    b->cell[4][6] = b->cell[5][6] = '#';
    b->team[0].mode = b->team[1].mode = ADVANCE;
    b->tank[0] = (Tank) {.id = 1, .side = 0, .x = 1, .y = 1, .hp = 3, .range = 4};
    b->tank[1] = (Tank) {.id = 2, .side = 0, .x = 1, .y = 6, .hp = 3, .range = 4};
    b->tank[2] = (Tank) {.id = 1, .side = 1, .x = 10, .y = 1, .hp = 3, .range = 4};
    b->tank[3] = (Tank) {.id = 2, .side = 1, .x = 10, .y = 6, .hp = 3, .range = 4};
}

static int read_cfg(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "Не удалось открыть сценарий: %s\n", strerror(errno));
        return 0;
    }
    size_t pos = 0;
    int ok = 1;
    for (;;) {
        ssize_t n = read(fd, buf + pos, cap - pos);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            dprintf(STDERR_FILENO, "Ошибка чтения сценария: %s\n", strerror(errno));
            ok = 0;
            break;
        }
        if (n == 0) {
            break;
        }
        pos += (size_t) n;
        if (pos == cap) {
            dprintf(STDERR_FILENO, "Сценарий слишком большой (максимум %zu байт).\n", cap - 1);
            ok = 0;
            break;
        }
    }
    if (close(fd) < 0) {
        dprintf(STDERR_FILENO, "Ошибка закрытия сценария: %s\n", strerror(errno));
        ok = 0;
    }
    if (memchr(buf, '\0', pos)) {
        dprintf(STDERR_FILENO, "Сценарий содержит нулевой байт. Нужен текстовый файл.\n");
        ok = 0;
    }
    if (ok) {
        buf[pos] = '\0';
    }
    return ok;
}

/* Разделение строк без пропуска пустых строк, в том числе внутри карты */
static char *next_line(char *buf, int *pos) {
    if (!buf[*pos]) {
        return NULL;
    }
    char *s = &buf[*pos];
    while (buf[*pos] && buf[*pos] != '\n') {
        ++(*pos);
    }
    if (buf[*pos] == '\n') {
        buf[*pos] = '\0';
        ++(*pos);
    }
    size_t n = strlen(s);
    if (n && s[n - 1] == '\r') {
        s[n - 1] = '\0';
    }
    return s;
}

static int cfg_err(int line, const char *msg) {
    dprintf(STDERR_FILENO, "Ошибка сценария, строка %d: %s\n", line, msg);
    return 0;
}

int cfg_load(const char *path, Cfg *cfg, Battle *b) {
    char buf[16384];
    if (!read_cfg(path, buf, sizeof(buf))) {
        return 0;
    }
    *b = (Battle) {0};
    b->team[0].mode = b->team[1].mode = ADVANCE;
    char *s;
    int pos = 0, line = 0, has_field = 0, has_map = 0;
    while ((s = next_line(buf, &pos))) {
        ++line;
        char *hash = strchr(s, '#');
        if (hash) {
            *hash = '\0';
        }
        char *arg[8];
        int n = 0;
        char *p = strtok(s, " \t");
        while (p && n < 8) {
            arg[n++] = p;
            p = strtok(NULL, " \t");
        }
        if (!n) {
            continue;
        }
        if (n == 8) {
            return cfg_err(line, "слишком много значений");
        }
        if (!strcmp(arg[0], "field")) {
            if (has_field || n != 3 || !num(arg[1], 1, MAX_W, &b->w)
                || !num(arg[2], 1, MAX_H, &b->h)) {
                return cfg_err(line, "ожидается одно поле: field <ширина 1..40> <высота 1..20>");
            }
            has_field = 1;
        } else if (!strcmp(arg[0], "map")) {
            if (n != 1 || !has_field || has_map) {
                return cfg_err(line, "map должна идти один раз после field");
            }
            for (int y = 0; y < b->h; ++y) {
                s = next_line(buf, &pos);
                ++line;
                if (!s || strlen(s) != (size_t) b->w) {
                    return cfg_err(line, "неверная длина строки карты");
                }
                for (int x = 0; x < b->w; ++x) {
                    if (s[x] != '.' && s[x] != '#') {
                        return cfg_err(line, "в карте разрешены только '.' и '#'");
                    }
                    b->cell[y][x] = s[x];
                }
            }
            has_map = 1;
        } else if (!strcmp(arg[0], "tank")) {
            Tank t = {0};
            if (!has_map || n != 7 || b->n == MAX_TANKS
                || (strcmp(arg[1], "A") && strcmp(arg[1], "B"))
                || !num(arg[2], 1, 99, &t.id)
                || !num(arg[3], 0, b->w - 1, &t.x) || !num(arg[4], 0, b->h - 1, &t.y)
                || !num(arg[5], 1, 100, &t.hp) || !num(arg[6], 1, MAX_W + MAX_H, &t.range)) {
                return cfg_err(line, "после карты: tank <A|B> <id 1..99> <x> <y> <hp 1..100> <range 1..60>; до 32 танков");
            }
            t.side = arg[1][0] - 'A';
            if (b->cell[t.y][t.x] == '#') {
                return cfg_err(line, "танк находится на препятствии");
            }
            for (int i = 0; i < b->n; ++i) {
                if (b->tank[i].x == t.x && b->tank[i].y == t.y) {
                    return cfg_err(line, "два танка находятся в одной клетке");
                }
                if (b->tank[i].side == t.side && b->tank[i].id == t.id) {
                    return cfg_err(line, "повторяется номер танка одной стороны");
                }
            }
            b->tank[b->n++] = t;
        } else if (!strcmp(arg[0], "team")) {
            if (n != 3 || (strcmp(arg[1], "A") && strcmp(arg[1], "B"))
                || (strcmp(arg[2], "hold") && strcmp(arg[2], "advance"))) {
                return cfg_err(line, "ожидается team <A|B> <hold|advance>");
            }
            b->team[arg[1][0] - 'A'].mode = !strcmp(arg[2], "hold") ? HOLD : ADVANCE;
        } else {
            int *dst = NULL, lo = 0, hi = 0;
            if (!strcmp(arg[0], "limit")) {
                dst = &cfg->limit;
                hi = 1000000;
            } else if (!strcmp(arg[0], "accuracy")) {
                dst = &cfg->acc;
                hi = 100;
            } else if (!strcmp(arg[0], "flight")) {
                dst = &cfg->fly;
                lo = 1;
                hi = MAX_FLY;
            } else if (!strcmp(arg[0], "seed")) {
                dst = &cfg->seed;
                hi = INT_MAX;
            } else if (!strcmp(arg[0], "delay")) {
                dst = &cfg->delay;
                hi = 10000;
            }
            if (!dst || n != 2 || !num(arg[1], lo, hi, dst)) {
                return cfg_err(line, "неизвестный параметр или недопустимое значение");
            }
        }
    }
    if (!has_field || !has_map) {
        return cfg_err(line, "необходимо задать field и map");
    }
    /* Номера задают стабильный порядок. перестановка строк tank не меняет бой */
    for (int i = 0; i < b->n; ++i) {
        for (int j = i + 1; j < b->n; ++j) {
            Tank *a = &b->tank[i], *c = &b->tank[j];
            if (a->side > c->side || (a->side == c->side && a->id > c->id)) {
                Tank tmp = *a;
                *a = *c;
                *c = tmp;
            }
        }
    }
    return 1;
}
