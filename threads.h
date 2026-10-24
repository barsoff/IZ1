#ifndef THREADS_H
#define THREADS_H

#include <pthread.h>
#include <semaphore.h>
#include "battle.h"

typedef struct Threads Threads;

/* каждый поток получает собственный аргумент, который живёт до завершения боя */
typedef struct {
    Threads *shared;
    int index;
} ThreadArg;

struct Threads {
    const Battle *battle;
    Plan plan[MAX_TANKS];
    pthread_t thread[MAX_TANKS + 2];
    ThreadArg arg[MAX_TANKS + 2];
    pthread_mutex_t lock;
    sem_t start[MAX_TANKS + 2]; /* сначала задания танкам, затем координаторам */
    sem_t done[3]; /* ответы танков стороны A, стороны B и обоих координаторов */
    int stop, failed;
    int created, start_count, done_count; /* счётчики для очистки при неполном запуске */
};

int threads_start(Threads *threads, const Battle *battle);
int threads_plan(Threads *threads, Plan plan[]);
int threads_stop(Threads *threads);
#endif
