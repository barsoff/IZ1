#include "net.h"
#include "output.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { PEERS = 32, OBS_MAX = PEERS - 3, WAIT = -1, OBS = 2, TX_N = 2 * (HDR + MSG_N), END_N = 1024 };

typedef struct {
    int fd, role, job, close, half;
    long long due;
    size_t rxn, sent, txn;
    unsigned char rx[HDR + PLAN_N];
    unsigned char tx[TX_N];
} Peer;

typedef struct {
    Peer peer[PEERS];
    Battle *b;
    Plan plan[MAX_TANKS];
    unsigned char job[JOB_N];
    unsigned char view[MSG_N];
    size_t jn, vn;
    int fd, out, active, ready[2], workers[2];
} Srv;

static long long ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static int nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

static void obs_send(Srv *s, unsigned type, const void *buf, size_t n);

static void side_status(Srv *s, int side, const char *state) {
    char msg[128];
    int n = snprintf(msg, sizeof(msg), "Сторона %c %s\n", 'A' + side, state);
    dprintf(STDOUT_FILENO, "%s", msg);
    obs_send(s, INFO, msg, (size_t) n);
}

static void drop(Srv *s, int i) {
    Peer *p = &s->peer[i];
    if (p->role == 0 || p->role == 1) {
        s->workers[p->role] = -1;
        s->ready[p->role] = 0;
        if (s->fd >= 0) side_status(s, p->role, "отключилась");
    }
    close(p->fd);
    p->fd = -1;
}

/* Сохраняем границы сообщений при частичной записи в сокет */
static int queue(Peer *p, unsigned type, const void *buf, size_t n) {
    size_t cap = sizeof(p->tx) - (type == END || type == ERR ? 0 : END_N);
    if (p->sent == p->txn) p->sent = p->txn = 0;
    if (p->txn + HDR + n > cap && p->sent) {
        memmove(p->tx, p->tx + p->sent, p->txn - p->sent);
        p->txn -= p->sent;
        p->sent = 0;
    }
    if (p->txn + HDR + n > cap) return 0;
    net_put(p->tx + p->txn, type);
    net_put(p->tx + p->txn + 4, (uint32_t) n);
    memcpy(p->tx + p->txn + HDR, buf, n);
    p->txn += HDR + n;
    return 1;
}

static void reject(Peer *p, const char *msg) {
    p->close = 1;
    p->due = ms() + 1000;
    queue(p, ERR, msg, strlen(msg));
}

static void job_send(Srv *s, int i) {
    Peer *p = &s->peer[i];
    if (!queue(p, JOB, s->job, s->jn)) {
        drop(s, i);
        return;
    }
    p->job = 1;
    p->due = ms() + 5000;
}

static void msg_recv(Srv *s, int i, unsigned type, size_t n) {
    Peer *p = &s->peer[i];
    const unsigned char *buf = p->rx + HDR;
    if (p->role == WAIT && type == HELLO && n == 1) {
        int role = buf[0] == 'O' ? OBS : (int) buf[0] - 'A';
        if (buf[0] != 'A' && buf[0] != 'B' && buf[0] != 'O') {
            reject(p, "Неизвестная роль\n");
            return;
        }
        if (role != OBS && s->workers[role] >= 0) {
            reject(p, "Сторона уже подключена\n");
            return;
        }
        if (role == OBS) {
            int cnt = 0;
            for (int j = 0; j < PEERS; ++j) {
                cnt += s->peer[j].fd >= 0 && s->peer[j].role == OBS;
            }
            if (cnt >= OBS_MAX) {
                reject(p, "Достигнут предел подключений наблюдателей\n");
                return;
            }
        }
        p->role = role;
        p->due = 0;
        if (role == OBS) {
            if (!queue(p, VIEW, s->view, s->vn)) drop(s, i);
        } else {
            s->workers[role] = i;
            side_status(s, role, "подключилась");
            if (s->active) job_send(s, i);
        }
        return;
    }
    if (p->job && type == PLANS && plan_read(buf, n, s->b, p->role, s->plan)) {
        s->ready[p->role] = 1;
        p->job = 0;
        p->due = 0;
        dprintf(STDOUT_FILENO, "Такт %llu: получен план стороны %c\n",
                s->b->step + 1, 'A' + p->role);
        return;
    }
    reject(p, "Некорректное сообщение\n");
}

static void peer_read(Srv *s, int i) {
    Peer *p = &s->peer[i];
    size_t n = p->rxn < HDR ? HDR - p->rxn : HDR + net_get(p->rx + 4) - p->rxn;
    ssize_t k = recv(p->fd, p->rx + p->rxn, n, 0);
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    if (k <= 0) {
        drop(s, i);
        return;
    }
    p->rxn += (size_t) k;
    if (p->rxn < HDR) return;
    n = net_get(p->rx + 4);
    if (n > PLAN_N) {
        reject(p, "Сообщение слишком велико\n");
        return;
    }
    if (p->rxn < HDR + n) return;
    msg_recv(s, i, net_get(p->rx), n);
    p->rxn = 0;
}

static void peer_write(Srv *s, int i) {
    Peer *p = &s->peer[i];
    ssize_t k = send(p->fd, p->tx + p->sent, p->txn - p->sent, MSG_NOSIGNAL);
    if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    if (k <= 0) {
        drop(s, i);
        return;
    }
    p->sent += (size_t) k;
}

static void peer_add(Srv *s) {
    int fd = accept(s->fd, NULL, NULL);
    if (fd < 0) return;
    if (!net_nodelay(fd) || !nonblock(fd)) {
        close(fd);
        return;
    }
    for (int i = 0; i < PEERS; ++i) {
        Peer *p = &s->peer[i];
        if (p->fd >= 0) continue;
        p->fd = fd;
        p->role = WAIT;
        p->job = p->close = p->half = 0;
        p->rxn = p->sent = p->txn = 0;
        p->due = ms() + 5000;
        return;
    }
    close(fd);
}

static int net_poll(Srv *s, int wait, int end) {
    struct pollfd fds[PEERS + 1] = {{.fd = s->fd, .events = POLLIN}};
    for (int i = 0; i < PEERS; ++i) {
        Peer *p = &s->peer[i];
        short ev = end || !p->close ? POLLIN : 0;
        if (p->sent < p->txn) ev |= POLLOUT;
        fds[i + 1] = (struct pollfd) {.fd = p->fd, .events = ev};
    }
    if (poll(fds, PEERS + 1, wait) < 0) return errno == EINTR;
    long long now = ms();
    for (int i = 0; i < PEERS; ++i) {
        Peer *p = &s->peer[i];
        short ev = fds[i + 1].revents;
        if (p->fd < 0) continue;
        if (ev & (POLLERR | POLLHUP | POLLNVAL)) {
            drop(s, i);
            continue;
        }
        if (ev & POLLIN) {
            if (end) {
                unsigned char buf[PLAN_N + HDR];
                ssize_t n = recv(p->fd, buf, sizeof(buf), 0);
                if (!n || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                    drop(s, i);
                    continue;
                }
            } else {
                peer_read(s, i);
            }
        }
        if (p->fd >= 0 && (ev & POLLOUT)) peer_write(s, i);
        if (p->fd < 0) continue;
        if ((p->due && now >= p->due) || (!end && p->close && p->sent == p->txn)) {
            drop(s, i);
        } else if (end && !p->half && p->sent == p->txn) {
            /* После итогов закрываем запись и ждём, пока клиент закроет сокет */
            if (shutdown(p->fd, SHUT_WR) < 0) drop(s, i);
            else p->half = 1;
        }
    }
    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return 0;
    if (fds[0].revents & POLLIN) peer_add(s);
    return 1;
}

/* Повторно используем вывод модели для сводного потока наблюдателей */
static int view_reset(int fd) {
    return ftruncate(fd, 0) == 0 && lseek(fd, 0, SEEK_SET) == 0;
}

static int view_read(Srv *s, size_t *n) {
    off_t len = lseek(s->out, 0, SEEK_CUR);
    if (len < 0 || len > MSG_N) return 0;
    size_t pos = 0;
    while (pos < (size_t) len) {
        ssize_t k = pread(s->out, s->view + pos, (size_t) len - pos, (off_t) pos);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        pos += (size_t) k;
    }
    *n = pos;
    return 1;
}

static void obs_send(Srv *s, unsigned type, const void *buf, size_t n) {
    for (int i = 0; i < PEERS; ++i) {
        Peer *p = &s->peer[i];
        if (p->fd >= 0 && p->role == OBS && !p->close && !queue(p, type, buf, n)) drop(s, i);
    }
}

static int view_field(Srv *s) {
    if (!view_reset(s->out)) return 0;
    out_field_fd(s->out, s->b);
    return view_read(s, &s->vn);
}

static void tick_start(Srv *s) {
    s->active = 1;
    s->jn = job_pack(s->job, s->b, s->b->step + 1);
    for (int side = 0; side < 2; ++side) job_send(s, s->workers[side]);
}

static int tick_done(Srv *s, const Cfg *cfg) {
    ++s->b->step;
    s->active = s->ready[0] = s->ready[1] = 0;
    if (!view_reset(s->out)) return 0;
    battle_step_fd(s->out, s->b, cfg, s->plan);
    if (!battle_valid(s->b)) return 0;
    out_field_fd(s->out, s->b);
    size_t n;
    if (!view_read(s, &n)) return 0;
    obs_send(s, VIEW, s->view, n);
    for (int side = 0; side < 2; ++side) {
        int i = s->workers[side];
        char msg[160];
        int len = snprintf(msg, sizeof(msg), "Такт %llu: план %c применён, в строю %d\n",
                           s->b->step, 'A' + side, alive(s->b, side));
        if (i >= 0 && !queue(&s->peer[i], INFO, msg, (size_t) len)) drop(s, i);
    }
    return view_field(s);
}

static void finish(Srv *s, End end) {
    size_t n = 0;
    int ok = view_reset(s->out) && out_stats(s->out, s->b, end) && view_read(s, &n) && n <= END_N - HDR;
    char msg[160];
    size_t len = (size_t) snprintf(msg, sizeof(msg), "Бой завершён: %s\n", out_end(end));
    unsigned type = end == FAIL ? ERR : END;
    long long due = ms() + 2000;
    for (int i = 0; i < PEERS; ++i) {
        Peer *p = &s->peer[i];
        if (p->fd < 0) continue;
        if (!p->close) {
            if (p->role == OBS && ok) queue(p, type, s->view, n);
            else queue(p, type, msg, len);
        }
        p->close = 1;
        p->due = due;
    }
    while (ms() < due) {
        int left = 0;
        for (int i = 0; i < PEERS; ++i) left += s->peer[i].fd >= 0;
        if (!left || !net_poll(s, 50, 1)) break;
    }
    for (int i = 0; i < PEERS; ++i) if (s->peer[i].fd >= 0) drop(s, i);
}

int net_run(const char *ip, const char *port, Battle *b, const Cfg *cfg, const char *log, const char *src,
            const volatile sig_atomic_t *stop) {
    int fd = net_open(ip, port, 1);
    if (fd < 0 || !nonblock(fd)) {
        dprintf(STDERR_FILENO, "Не удалось открыть сервер %s:%s: %s\n", ip, port, strerror(errno));
        if (fd >= 0) close(fd);
        return 1;
    }
    FILE *out = tmpfile();
    Srv *s = calloc(1, sizeof(*s));
    if (!out || !s) {
        dprintf(STDERR_FILENO, "Не удалось подготовить вывод сервера\n");
        if (out) fclose(out);
        free(s);
        close(fd);
        return 1;
    }
    int logfd = out_open(log, src);
    if (logfd < 0) {
        fclose(out);
        free(s);
        close(fd);
        return 1;
    }
    s->fd = fd;
    s->out = fileno(out);
    s->b = b;
    s->workers[0] = s->workers[1] = -1;
    for (int i = 0; i < PEERS; ++i) s->peer[i].fd = -1;
    dprintf(STDOUT_FILENO, "Сервер готов на %s:%s, ожидает стороны A и B\n", ip, port);
    End end = view_field(s) ? battle_end(b, cfg) : FAIL;
    long long next = ms() + cfg->delay;
    while (end == RUN && !*stop) {
        int both = s->workers[0] >= 0 && s->workers[1] >= 0;
        int wait = 1000;
        if (both && !s->active) {
            long long left = next - ms();
            wait = left <= 0 ? 0 : left < wait ? (int) left : wait;
        }
        if (!net_poll(s, wait, 0)) {
            end = FAIL;
            break;
        }
        if (*stop) break;
        if (s->ready[0] && s->ready[1]) {
            end = tick_done(s, cfg) ? battle_end(b, cfg) : FAIL;
            next = ms() + cfg->delay;
        } else if (!s->active && s->workers[0] >= 0 && s->workers[1] >= 0 && ms() >= next) {
            tick_start(s);
        }
    }
    if (*stop && end == RUN) end = STOP;
    close(fd);
    s->fd = -1;
    finish(s, end);
    int ok = out_stats(logfd, b, end);
    if (close(logfd) < 0) ok = 0;
    fclose(out);
    free(s);
    if (!ok || end == FAIL) return 1;
    return *stop ? 128 + *stop : 0;
}
