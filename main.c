#include "battle.h"
#include "config.h"
#include "output.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop = 0;

/* Обработчик только запоминает сигнал */
static void on_stop(int sig) {
    stop = sig;
}

static void help(void) {
    dprintf(STDOUT_FILENO,
            "Танковые бои, вариант 24\n"
            "Использование: tanks [параметры]\n"
            "  --config PATH   текстовый сценарий (по умолчанию встроенный пример)\n"
            "  --log PATH      файл итогов (по умолчанию battle.log; перезаписывается)\n"
            "  --seed N        начальное значение генератора, 0..2147483647\n"
            "  --delay N       задержка между тактами, 0..10000 мс\n"
            "  --limit N       предел тактов, 0..1000000; 0 — до победы или Ctrl+C\n"
            "  --help          показать справку\n"
            "Параметры командной строки имеют приоритет над файлом.\n");
}

int main(int argc, char **argv) {
    Cfg cfg;
    Battle b;
    cfg_default(&cfg, &b);
    const char *src = NULL, *log = "battle.log";
    int seed = -1, delay = -1, limit = -1;
    for (int i = 1; i < argc; ++i) {
        const char *key = argv[i];
        if (!strcmp(key, "--help")) {
            help();
            return 0;
        }
        if (strcmp(key, "--config") && strcmp(key, "--log") && strcmp(key, "--seed")
            && strcmp(key, "--delay") && strcmp(key, "--limit")) {
            dprintf(STDERR_FILENO, "Неизвестный аргумент: %s. См. --help.\n", key);
            return 1;
        }
        if (++i == argc || !argv[i][0]) {
            dprintf(STDERR_FILENO, "Не задано значение %s.\n", key);
            return 1;
        }
        if (!strcmp(key, "--config")) {
            src = argv[i];
        } else if (!strcmp(key, "--log")) {
            log = argv[i];
        } else {
            int *dst = !strcmp(key, "--seed") ? &seed : (!strcmp(key, "--delay") ? &delay : &limit);
            int hi = dst == &seed ? INT_MAX : (dst == &delay ? 10000 : 1000000);
            if (!num(argv[i], 0, hi, dst)) {
                dprintf(STDERR_FILENO, "Недопустимое значение %s: %s (нужно 0..%d).\n", key, argv[i], hi);
                return 1;
            }
        }
    }
    if (src && !cfg_load(src, &cfg, &b)) return 1;
    if (seed >= 0) cfg.seed = seed;
    if (delay >= 0) cfg.delay = delay;
    if (limit >= 0) cfg.limit = limit;

    struct sigaction sa = {0};
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        dprintf(STDERR_FILENO, "Не удалось установить обработчик сигнала.\n");
        return 1;
    }
    /* Закрытие получателя stdout не должно мешать записи в журнал */
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0) {
        dprintf(STDERR_FILENO, "Не удалось настроить SIGPIPE.\n");
        return 1;
    }
    int fd = out_open(log, src);
    if (fd < 0) return 1;
    srand((unsigned) cfg.seed);
    dprintf(STDOUT_FILENO, "Танковые бои — вариант 24\n"
             "Поле %dx%d; seed=%d; точность=%d%%; полёт=%d; лимит=%d; задержка=%d мс.\n"
             "Стратегии: A=%s, B=%s. Стрельба навесная; урон одного попадания — 1.\n"
             "Обозначения: . — свободно, # — препятствие, A/B — танки, X — обломки.\n"
             "Координаты (x, y) начинаются с нуля. Остановка: Ctrl+C.\n",
             b.w, b.h, cfg.seed, cfg.acc, cfg.fly, cfg.limit, cfg.delay,
             b.team[0].mode == HOLD ? "hold" : "advance", b.team[1].mode == HOLD ? "hold" : "advance");
    out_field(&b);
    End end = battle_end(&b, &cfg);
    /* Один цикл имитирует действия всех танков; фоновые потоки не создаются */
    while (end == RUN && !stop) {
        if (cfg.delay) {
            struct timespec ts = {.tv_sec = cfg.delay / 1000, .tv_nsec = (cfg.delay % 1000) * 1000000L};
            while (nanosleep(&ts, &ts) < 0) {
                if (errno != EINTR) {
                    dprintf(STDERR_FILENO, "Ошибка задержки: %s\n", strerror(errno));
                    end = FAIL;
                    break;
                }
                if (stop) break;
            }
        }
        if (stop || end == FAIL) break;
        battle_step(&b, &cfg);
        if (!battle_valid(&b)) {
            dprintf(STDERR_FILENO, "Ошибка: нарушены правила размещения танков или хранения снарядов.\n");
            end = FAIL;
            break;
        }
        out_field(&b);
        end = battle_end(&b, &cfg);
    }
    if (stop) end = STOP;
    /* При любом результате, включая сигнал, сохраняем итог и закрываем файл */
    int ok = out_stats(fd, &b, end);
    if (!ok) dprintf(STDERR_FILENO, "Ошибка записи итогов: %s\n", strerror(errno));
    if (!out_stats(STDOUT_FILENO, &b, end)) ok = 0;
    if (close(fd) < 0) {
        dprintf(STDERR_FILENO, "Ошибка закрытия журнала: %s\n", strerror(errno));
        ok = 0;
    }
    if (!ok || end == FAIL) return 1;
    return stop ? 128 + stop : 0;
}
