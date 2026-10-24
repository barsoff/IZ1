#include "threads.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void report_error(const char *name, int error) {
    dprintf(STDERR_FILENO, "Ошибка %s: %s\n", name, strerror(error));
}

static int stopped(Threads *t) {
    pthread_mutex_lock(&t->lock);
    int stop = t->stop;
    pthread_mutex_unlock(&t->lock);
    return stop;
}

/* при ошибке пробуждаем участников, чтобы они не остались ждать ответа */
static void runtime_error(Threads *t, const char *name, int error) {
    pthread_mutex_lock(&t->lock);
    if (t->failed) {
        pthread_mutex_unlock(&t->lock);
        return;
    }
    t->failed = 1;
    t->stop = 1;
    pthread_mutex_unlock(&t->lock);
    report_error(name, error);
    for (int i = 0; i < t->start_count; ++i) {
        sem_post(&t->start[i]);
    }
    for (int i = 0; i < t->done_count; ++i) {
        for (int j = 0; j < MAX_TANKS + 2; ++j) {
            sem_post(&t->done[i]);
        }
    }
}

static int wait_sem(Threads *t, sem_t *sem) {
    while (sem_wait(sem) < 0) {
        if (errno != EINTR) {
            runtime_error(t, "sem_wait", errno);
            return 0;
        }
    }
    return 1;
}

static int post_sem(Threads *t, sem_t *sem) {
    if (sem_post(sem) < 0) {
        runtime_error(t, "sem_post", errno);
        return 0;
    }
    return 1;
}

static void *tank_main(void *data) {
    ThreadArg *arg = data;
    Threads *t = arg->shared;
    int index = arg->index;
    int side = t->battle->tank[index].side;
    while (wait_sem(t, &t->start[index])) {
        if (stopped(t)) {
            break;
        }
        Plan plan;
        tank_plan(t->battle, index, &plan);
        /* расчёт выполняется параллельно, только публикация требует мьютекса */
        pthread_mutex_lock(&t->lock);
        t->plan[index] = plan;
        pthread_mutex_unlock(&t->lock);
        if (!post_sem(t, &t->done[side])) {
            break;
        }
    }
    return NULL;
}

static void *team_main(void *data) {
    ThreadArg *arg = data;
    Threads *t = arg->shared;
    int side = arg->index;
    int start = t->battle->n + side;
    while (wait_sem(t, &t->start[start])) {
        if (stopped(t)) {
            break;
        }
        int jobs = 0;
        for (int i = 0; i < t->battle->n; ++i) {
            const Tank *tank = &t->battle->tank[i];
            if (tank->side == side && tank->hp > 0) {
                if (!post_sem(t, &t->start[i])) {
                    return NULL;
                }
                ++jobs;
            }
        }
        for (int i = 0; i < jobs; ++i) {
            if (!wait_sem(t, &t->done[side]) || stopped(t)) {
                return NULL;
            }
        }
        /* общий семафор получает по одному ответу от каждой стороны */
        if (!post_sem(t, &t->done[2])) {
            break;
        }
    }
    return NULL;
}

int threads_start(Threads *t, const Battle *battle) {
    *t = (Threads) {.battle = battle};
    int error = pthread_mutex_init(&t->lock, NULL);
    if (error) {
        report_error("pthread_mutex_init", error);
        return 0;
    }
    for (int i = 0; i < battle->n + 2; ++i) {
        if (sem_init(&t->start[i], 0, 0) < 0) {
            report_error("sem_init", errno);
            threads_stop(t);
            return 0;
        }
        ++t->start_count;
    }
    for (int i = 0; i < 3; ++i) {
        if (sem_init(&t->done[i], 0, 0) < 0) {
            report_error("sem_init", errno);
            threads_stop(t);
            return 0;
        }
        ++t->done_count;
    }
    /* рабочие потоки наследуют блокировку сигналов остановки */
    sigset_t signals, previous;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    error = pthread_sigmask(SIG_BLOCK, &signals, &previous);
    if (error) {
        report_error("pthread_sigmask", error);
        threads_stop(t);
        return 0;
    }
    for (int i = 0; i < battle->n; ++i) {
        t->arg[i] = (ThreadArg) {.shared = t, .index = i};
        error = pthread_create(&t->thread[i], NULL, tank_main, &t->arg[i]);
        if (error) {
            break;
        }
        ++t->created;
    }
    if (!error) {
        for (int side = 0; side < 2; ++side) {
            int i = battle->n + side;
            t->arg[i] = (ThreadArg) {.shared = t, .index = side};
            error = pthread_create(&t->thread[i], NULL, team_main, &t->arg[i]);
            if (error) {
                break;
            }
            ++t->created;
        }
    }
    /* главный поток снова может принимать сигналы */
    int mask_error = pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (error) {
        report_error("pthread_create", error);
    }
    if (mask_error) {
        report_error("pthread_sigmask", mask_error);
    }
    if (error || mask_error) {
        threads_stop(t);
        return 0;
    }
    return 1;
}

int threads_plan(Threads *t, Plan plan[]) {
    int n = t->battle->n;
    for (int side = 0; side < 2; ++side) {
        if (!post_sem(t, &t->start[n + side])) {
            return 0;
        }
    }
    for (int side = 0; side < 2; ++side) {
        if (!wait_sem(t, &t->done[2]) || stopped(t)) {
            return 0;
        }
    }
    pthread_mutex_lock(&t->lock);
    memcpy(plan, t->plan, sizeof(t->plan));
    pthread_mutex_unlock(&t->lock);
    return 1;
}

int threads_stop(Threads *t) {
    int ok = 1;
    pthread_mutex_lock(&t->lock);
    t->stop = 1;
    pthread_mutex_unlock(&t->lock);
    for (int i = 0; i < t->created; ++i) {
        if (sem_post(&t->start[i]) < 0) {
            report_error("sem_post", errno);
            ok = 0;
        }
    }
    for (int i = 0; i < t->created; ++i) {
        int error = pthread_join(t->thread[i], NULL);
        if (error) {
            report_error("pthread_join", error);
            ok = 0;
        }
    }
    if (t->failed) {
        ok = 0;
    }
    for (int i = 0; i < t->start_count; ++i) {
        if (sem_destroy(&t->start[i]) < 0) {
            report_error("sem_destroy", errno);
            ok = 0;
        }
    }
    for (int i = 0; i < t->done_count; ++i) {
        if (sem_destroy(&t->done[i]) < 0) {
            report_error("sem_destroy", errno);
            ok = 0;
        }
    }
    int error = pthread_mutex_destroy(&t->lock);
    if (error) {
        report_error("pthread_mutex_destroy", error);
        ok = 0;
    }
    return ok;
}
