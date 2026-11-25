#include "net.h"
#include "config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

void net_put(unsigned char *buf, uint32_t val) {
    val = htonl(val);
    memcpy(buf, &val, sizeof(val));
}

uint32_t net_get(const unsigned char *buf) {
    uint32_t val;
    memcpy(&val, buf, sizeof(val));
    return ntohl(val);
}

int net_nodelay(int fd) {
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
}

int net_open(const char *ip, const char *port, int server) {
    int pn;
    struct sockaddr_in addr = {.sin_family = AF_INET};
    if (!num(port, 1, 65535, &pn) || inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        dprintf(STDERR_FILENO, "Нужны IPv4-адрес и порт 1..65535\n");
        return -1;
    }
    addr.sin_port = htons((uint16_t) pn);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    int ok = server ? setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0
                       && bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0
                       && listen(fd, 32) == 0
                    : net_nodelay(fd) && connect(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0;
    if (!ok) {
        int err = errno;
        close(fd);
        errno = err;
        return -1;
    }
    return fd;
}

static int send_all(int fd, const unsigned char *buf, size_t n) {
    while (n) {
        ssize_t k = send(fd, buf, n, MSG_NOSIGNAL);
        if (k <= 0) return 0;
        buf += k;
        n -= (size_t) k;
    }
    return 1;
}

int net_send(int fd, unsigned type, const void *buf, size_t n) {
    unsigned char hdr[HDR];
    net_put(hdr, type);
    net_put(hdr + 4, (uint32_t) n);
    return send_all(fd, hdr, sizeof(hdr)) && send_all(fd, buf, n);
}

static int read_all(int fd, unsigned char *buf, size_t n) {
    while (n) {
        ssize_t k = recv(fd, buf, n, 0);
        if (k <= 0) return 0;
        buf += k;
        n -= (size_t) k;
    }
    return 1;
}

int net_read(int fd, unsigned *type, void *buf, size_t cap, size_t *n) {
    unsigned char hdr[HDR];
    if (!read_all(fd, hdr, sizeof(hdr))) return 0;
    *type = net_get(hdr);
    *n = net_get(hdr + 4);
    return *n <= cap && read_all(fd, buf, *n);
}

/* Передаём только поля, нужные для расчёта намерений */
size_t job_pack(unsigned char *buf, const Battle *b, unsigned long long step) {
    uint32_t hdr[] = {(uint32_t) (step >> 32), (uint32_t) step, b->w, b->h, b->n, b->team[0].mode, b->team[1].mode};
    for (int i = 0; i < 7; ++i) net_put(buf + i * 4, hdr[i]);
    size_t n = sizeof(hdr);
    for (int y = 0; y < b->h; ++y) {
        memcpy(buf + n, b->cell[y], (size_t) b->w);
        n += (size_t) b->w;
    }
    for (int i = 0; i < b->n; ++i) {
        const Tank *t = &b->tank[i];
        uint32_t val[] = {t->id, t->side, t->x, t->y, t->hp, t->range};
        for (int j = 0; j < 6; ++j, n += 4) net_put(buf + n, val[j]);
    }
    return n;
}

int job_read(const unsigned char *buf, size_t n, Battle *b) {
    if (n < 28) return 0;
    uint32_t w = net_get(buf + 8), h = net_get(buf + 12), nt = net_get(buf + 16);
    uint32_t a = net_get(buf + 20), c = net_get(buf + 24);
    if (!w || w > MAX_W || !h || h > MAX_H || nt > MAX_TANKS || a > ADVANCE || c > ADVANCE
        || n != 28 + w * h + 24 * nt) return 0;
    *b = (Battle) {.step = (unsigned long long) net_get(buf) << 32 | net_get(buf + 4), .w = (int) w, .h = (int) h, .n = (int) nt};
    b->team[0].mode = a;
    b->team[1].mode = c;
    size_t pos = 28;
    for (int y = 0; y < b->h; ++y, pos += w) memcpy(b->cell[y], buf + pos, w);
    for (int i = 0; i < b->n; ++i) {
        uint32_t val[6];
        for (int j = 0; j < 6; ++j, pos += 4) val[j] = net_get(buf + pos);
        if (!val[0] || val[0] > 99 || val[1] > 1 || val[2] >= w || val[3] >= h
            || val[4] > 100 || !val[5] || val[5] > MAX_W + MAX_H) return 0;
        b->tank[i] = (Tank) {(int) val[0], (int) val[1], (int) val[2],
                            (int) val[3], (int) val[4], (int) val[5]};
    }
    return battle_valid(b);
}

size_t plan_pack(unsigned char *buf, const Battle *b, int side, const Plan plan[]) {
    net_put(buf, (uint32_t) (b->step >> 32));
    net_put(buf + 4, (uint32_t) b->step);
    size_t n = 8;
    for (int i = 0; i < b->n; ++i) {
        if (b->tank[i].side != side) continue;
        const Plan *p = &plan[i];
        net_put(buf + n, (uint32_t) p->x);
        net_put(buf + n + 4, (uint32_t) p->y);
        net_put(buf + n + 8, (uint32_t) (p->dst + 1));
        n += 12;
    }
    return n;
}

int plan_read(const unsigned char *buf, size_t n, const Battle *b, int side, Plan plan[]) {
    if (n < 8) return 0;
    unsigned long long step = (unsigned long long) net_get(buf) << 32 | net_get(buf + 4);
    if (step != b->step + 1) return 0;
    size_t pos = 8;
    for (int i = 0; i < b->n; ++i) {
        if (b->tank[i].side != side) continue;
        if (pos + 12 > n) return 0;
        uint32_t x = net_get(buf + pos), y = net_get(buf + pos + 4), dst = net_get(buf + pos + 8);
        if (x >= (unsigned) b->w || y >= (unsigned) b->h || dst > (unsigned) b->n) return 0;
        Plan p = {.x = (int) x, .y = (int) y, .dst = (int) dst - 1};
        if (!plan_valid(b, i, &p)) return 0;
        plan[i] = p;
        pos += 12;
    }
    return pos == n;
}
