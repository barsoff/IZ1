#ifndef BATTLE_H
#define BATTLE_H

#define MAX_W 40
#define MAX_H 20
#define MAX_TANKS 32
#define MAX_FLY 20
/* В каждом такте каждый танк выпускает не более одного снаряда */
#define MAX_SHOTS (MAX_TANKS * (MAX_FLY + 1))

typedef enum { HOLD, ADVANCE } Mode;
typedef enum { RUN, WIN_A, WIN_B, DRAW, LIMIT, STOP, FAIL } End;

typedef struct {
    int id, side;
    int x, y;
    int hp, range;
} Tank;

/* Координатор стороны хранит общую стратегию и статистику */
typedef struct {
    Mode mode;
    unsigned long long fired, hit, miss;
} Team;

typedef struct {
    int src; /* Индекс стрелявшего танка */
    int x, y; /* Клетка, выбранная при выстреле */
    int hit; /* Результат проверки точности */
    unsigned long long due; /* Такт, в котором снаряд достигнет клетки */
} Shot;

typedef struct {
    int w, h;
    char cell[MAX_H][MAX_W];
    Tank tank[MAX_TANKS];
    int n;
    Team team[2];
    Shot shot[MAX_SHOTS];
    int ns;
    unsigned long long step;
} Battle;

typedef struct {
    int limit; /* 0 — работа до победы или прерывания */
    int acc, fly;
    int seed, delay; /* Задержка вывода в миллисекундах */
} Cfg;

void battle_step(Battle *b, const Cfg *cfg);
End battle_end(const Battle *b, const Cfg *cfg);
int alive(const Battle *b, int side);
int battle_valid(const Battle *b);

#endif
