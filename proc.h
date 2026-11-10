#ifndef PROC_H
#define PROC_H

#include <semaphore.h>
#include <signal.h>
#include <sys/types.h>
#include "battle.h"

enum { START = 0, DONE = 2, READY = 4, EXIT = 6, SLOT = 8, SEM_N = 10 };

typedef struct {
    Battle b;
    Plan plan[MAX_TANKS];
    sem_t sem[SEM_N];
    pid_t owner;
    int quit;
} Shared;

typedef struct {
    Shared *mem;
    sem_t *sem[SEM_N];
    pid_t pid[2];
    int ready[2];
    char name[64];
    int named, ext, count, made;
    volatile sig_atomic_t *stop;
} Proc;

int proc_start(Proc *p, int named, int ext, const char *name, volatile sig_atomic_t *stop);
int proc_plan(Proc *p, const Battle *b, Plan plan[]);
int proc_close(Proc *p);
int proc_worker(const char *name, int side);

#endif
