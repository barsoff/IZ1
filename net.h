#ifndef NET_H
#define NET_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include "battle.h"

/* Заголовок TCP сообщения содержит тип и длину тела */
enum { HELLO = 1, JOB, PLANS, VIEW, INFO, END, ERR };
enum { HDR = 8, PLAN_N = 8 + 12 * MAX_TANKS, JOB_N = 28 + MAX_W * MAX_H + 24 * MAX_TANKS };
enum { MSG_N = 128 * 1024 };

void net_put(unsigned char *buf, uint32_t val);
uint32_t net_get(const unsigned char *buf);
int net_nodelay(int fd);
int net_open(const char *ip, const char *port, int server);
int net_send(int fd, unsigned type, const void *buf, size_t n);
int net_read(int fd, unsigned *type, void *buf, size_t cap, size_t *n);
size_t job_pack(unsigned char *buf, const Battle *b, unsigned long long step);
int job_read(const unsigned char *buf, size_t n, Battle *b);
size_t plan_pack(unsigned char *buf, const Battle *b, int side, const Plan plan[]);
int plan_read(const unsigned char *buf, size_t n, const Battle *b, int side, Plan plan[]);
int net_run(const char *ip, const char *port, Battle *b, const Cfg *cfg, const char *log, const char *src,
            const volatile sig_atomic_t *stop);

#endif
