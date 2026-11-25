#include "net.h"
#include "output.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_stop(int sig) {
    stop = sig;
}

static int show(const unsigned char *buf, size_t n) {
    while (n) {
        ssize_t k = write(STDOUT_FILENO, buf, n);
        if (k <= 0) return 0;
        buf += k;
        n -= (size_t) k;
    }
    return 1;
}

int main(int argc, char **argv) {
    int side = -1;
    if (!WATCH && argc == 4 && (!strcmp(argv[3], "A") || !strcmp(argv[3], "B"))) {
        side = argv[3][0] - 'A';
    }
    if ((argc == 2 && !strcmp(argv[1], "--help")) || (WATCH ? argc != 3 : side < 0)) {
        dprintf(STDOUT_FILENO, "Запуск: %s IP PORT%s\n", argv[0], WATCH ? "" : " A|B");
        return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 1;
    }
    struct sigaction sa = {.sa_handler = on_stop};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) return 1;
    sa.sa_handler = SIG_IGN;
    if (sigaction(SIGPIPE, &sa, NULL) < 0) return 1;
    int fd = net_open(argv[1], argv[2], 0);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "Не удалось подключиться к %s:%s: %s\n", argv[1], argv[2], strerror(errno));
        return stop ? 128 + stop : 1;
    }
    unsigned char role = WATCH ? 'O' : 'A' + side;
    int ok = 0;
    if (net_send(fd, HELLO, &role, 1)) {
        unsigned char buf[MSG_N];
        unsigned type;
        size_t n;
        while (!stop && net_read(fd, &type, buf, sizeof(buf), &n)) {
            if (type == END || type == ERR) {
                ok = show(buf, n) && type == END;
                break;
            }
            if (type == INFO || (WATCH && type == VIEW)) {
                if (!show(buf, n)) break;
            } else if (!WATCH && type == JOB) {
                Battle b;
                if (!job_read(buf, n, &b)) break;
                unsigned char msg[PLAN_N];
                Plan plan[MAX_TANKS];
                for (int i = 0; i < b.n; ++i) {
                    if (b.tank[i].side == side) tank_plan(&b, i, &plan[i]);
                }
                size_t pn = plan_pack(msg, &b, side, plan);
                if (!net_send(fd, PLANS, msg, pn)) break;
                out_plans_fd(STDOUT_FILENO, &b, plan, side);
            } else {
                break;
            }
        }
    }
    close(fd);
    if (!ok && !stop) dprintf(STDERR_FILENO, "Сеанс завершился без подтверждения окончания боя\n");
    return stop ? 128 + stop : !ok;
}
