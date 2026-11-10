#include "proc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t quit;

static void on_stop(int sig) {
    quit = sig;
}

static int error(const char *msg) {
    dprintf(STDERR_FILENO, "Ошибка %s: %s\n", msg, strerror(errno));
    return 0;
}

static void sem_name(const Proc *p, int i, char name[]) {
    snprintf(name, 80, "%s_s%d", p->name, i);
}

/* Короткое ожидание позволяет заметить сигнал и потерю участника */
static int wait_sem(Proc *p, int i, int sec, int worker, const struct timespec *end) {
    struct timespec limit, now, ts;
    if (!end && sec) {
        if (clock_gettime(CLOCK_MONOTONIC, &limit) < 0) return error("clock_gettime");
        limit.tv_sec += sec;
        end = &limit;
    }
    for (;;) {
        if ((p->stop && *p->stop) || (worker && quit)) return 0;
        if (worker && kill(p->mem->owner, 0) < 0 && errno == ESRCH) return 0;
        if (clock_gettime(CLOCK_REALTIME, &ts) < 0) return error("clock_gettime");
        ts.tv_nsec += 100000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ++ts.tv_sec;
            ts.tv_nsec -= 1000000000L;
        }
        if (sem_timedwait(p->sem[i], &ts) == 0) return 1;
        if (errno != EINTR && errno != ETIMEDOUT) return error("sem_timedwait");
        if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) return error("clock_gettime");
        if (end && (now.tv_sec > end->tv_sec
            || (now.tv_sec == end->tv_sec && now.tv_nsec >= end->tv_nsec))) {
            errno = ETIMEDOUT;
            return error("ожидания участника");
        }
    }
}

static int post_sem(Proc *p, int i) {
    return sem_post(p->sem[i]) == 0 ? 1 : error("sem_post");
}

/* Сторона читает общий снимок и пишет только планы своих танков */
static int team_run(Proc *p, int side) {
    struct sigaction sa = {0};
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) < 0 || sigaction(SIGTERM, &sa, NULL) < 0) {
        return error("sigaction");
    }
    if (sem_trywait(p->sem[SLOT + side]) < 0) return error("сторона уже занята");
    int ok = post_sem(p, READY + side);
    while (ok && wait_sem(p, START + side, 0, 1, NULL)) {
        if (p->mem->quit) break;
        const Battle *b = &p->mem->b;
        for (int i = 0; i < b->n; ++i) {
            if (b->tank[i].side == side) tank_plan(b, i, &p->mem->plan[i]);
        }
        ok = post_sem(p, DONE + side);
    }
    if (quit) {
        /* Прерывание любой стороны завершает весь бой */
        kill(p->mem->owner, quit);
    }
    return post_sem(p, EXIT + side) && ok;
}

int proc_close(Proc *p) {
    int ok = 1;
    if (p->mem) {
        p->mem->quit = 1;
        for (int side = 0; side < 2; ++side) {
            if (p->sem[START + side] && !post_sem(p, START + side)) ok = 0;
        }
    }
    for (int side = 0; side < 2; ++side) {
        if (p->pid[side] > 0) {
            int status;
            pid_t res;
            /* Даём процессу завершиться, затем убираем зависшего участника */
            for (int i = 0; i < 100; ++i) {
                res = waitpid(p->pid[side], &status, WNOHANG);
                if (res > 0 || (res < 0 && errno != EINTR)) break;
                struct timespec ts = {.tv_nsec = 20000000L};
                nanosleep(&ts, NULL);
            }
            if (res == 0 || (res < 0 && errno == EINTR)) {
                dprintf(STDERR_FILENO, "Дочерний процесс не завершился, принудительная остановка\n");
                kill(p->pid[side], SIGKILL);
                do {
                    res = waitpid(p->pid[side], &status, 0);
                } while (res < 0 && errno == EINTR);
                ok = 0;
            }
            if (res < 0 || !WIFEXITED(status) || WEXITSTATUS(status)) ok = 0;
        }
    }
    p->stop = NULL;
    if (p->ext) {
        for (int side = 0; side < 2; ++side) {
            if (p->ready[side] && !wait_sem(p, EXIT + side, 2, 0, NULL)) ok = 0;
        }
    }
    for (int i = 0; i < p->count; ++i) {
        if (p->named) {
            if (sem_close(p->sem[i]) < 0) ok = error("sem_close");
            char name[80];
            sem_name(p, i, name);
            if (sem_unlink(name) < 0) ok = error("sem_unlink");
        } else if (sem_destroy(p->sem[i]) < 0) {
            ok = error("sem_destroy");
        }
    }
    if (p->mem && munmap(p->mem, sizeof(Shared)) < 0) ok = error("munmap");
    if (p->made && shm_unlink(p->name) < 0) ok = error("shm_unlink");
    return ok;
}

int proc_start(Proc *p, int named, int ext, const char *name, volatile sig_atomic_t *stop) {
    *p = (Proc) {.named = named, .ext = ext, .stop = stop};
    if (!name || name[0] != '/' || !name[1] || strchr(name + 1, '/') || strlen(name) >= sizeof(p->name)) {
        errno = EINVAL;
        return error("имени IPC");
    }
    strcpy(p->name, name);
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return error("shm_open");
    p->made = 1;
    int ok = ftruncate(fd, sizeof(Shared)) == 0;
    if (ok) {
        p->mem = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p->mem == MAP_FAILED) p->mem = NULL;
        ok = p->mem != NULL;
    }
    if (close(fd) < 0) ok = 0;
    if (!ok) {
        error("создания памяти");
        proc_close(p);
        return 0;
    }
    *p->mem = (Shared) {.owner = getpid()};
    for (int i = 0; i < SEM_N; ++i) {
        if (named) {
            char sn[80];
            sem_name(p, i, sn);
            p->sem[i] = sem_open(sn, O_CREAT | O_EXCL, 0600, i >= SLOT ? 1 : 0);
            if (p->sem[i] == SEM_FAILED) p->sem[i] = NULL;
            ok = p->sem[i] != NULL;
        } else {
            p->sem[i] = &p->mem->sem[i];
            ok = sem_init(p->sem[i], 1, i >= SLOT ? 1 : 0) == 0;
            if (!ok) p->sem[i] = NULL;
        }
        if (!ok) {
            error("создания семафора");
            proc_close(p);
            return 0;
        }
        ++p->count;
    }
    if (ext) {
        dprintf(STDERR_FILENO, "IPC готов: %s, запустите tanks-team %s A и tanks-team %s B\n", name, name, name);
    } else {
        for (int side = 0; side < 2; ++side) {
            pid_t pid = fork();
            if (pid == 0) _exit(team_run(p, side) ? 0 : 1);
            if (pid < 0) {
                error("fork");
                proc_close(p);
                return 0;
            }
            p->pid[side] = pid;
        }
    }
    struct timespec end;
    if (clock_gettime(CLOCK_MONOTONIC, &end) < 0) {
        error("clock_gettime");
        proc_close(p);
        return 0;
    }
    end.tv_sec += ext ? 30 : 5;
    for (int side = 0; side < 2; ++side) {
        if (!wait_sem(p, READY + side, 0, 0, &end)) {
            proc_close(p);
            return 0;
        }
        p->ready[side] = 1;
    }
    return 1;
}

int proc_plan(Proc *p, const Battle *b, Plan plan[]) {
    p->mem->b = *b;
    for (int side = 0; side < 2; ++side) {
        if (!post_sem(p, START + side)) return 0;
    }
    for (int side = 0; side < 2; ++side) {
        if (!wait_sem(p, DONE + side, 5, 0, NULL)) return 0;
    }
    memcpy(plan, p->mem->plan, sizeof(p->mem->plan));
    return 1;
}

int proc_worker(const char *name, int side) {
    Proc p = {.named = 1};
    if (strlen(name) >= sizeof(p.name)) return 1;
    strcpy(p.name, name);
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return !error("shm_open");
    struct stat st;
    int ok = fstat(fd, &st) == 0;
    if (ok && st.st_size != sizeof(Shared)) {
        dprintf(STDERR_FILENO, "Несовместимый размер разделяемой памяти\n");
        close(fd);
        return 1;
    }
    if (ok) {
        p.mem = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p.mem == MAP_FAILED) p.mem = NULL;
        ok = p.mem != NULL;
    }
    close(fd);
    if (!ok) return !error("подключения памяти");
    for (int i = 0; i < SEM_N; ++i) {
        char sn[80];
        sem_name(&p, i, sn);
        p.sem[i] = sem_open(sn, 0);
        if (p.sem[i] == SEM_FAILED) {
            error("sem_open");
            ok = 0;
            break;
        }
        ++p.count;
    }
    if (ok) ok = team_run(&p, side);
    for (int i = 0; i < p.count; ++i) {
        if (sem_close(p.sem[i]) < 0) ok = error("sem_close");
    }
    if (munmap(p.mem, sizeof(Shared)) < 0) ok = error("munmap");
    return ok ? 0 : 1;
}
