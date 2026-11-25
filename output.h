#ifndef OUTPUT_H
#define OUTPUT_H

#include "battle.h"

int out_open(const char *path, const char *src);
void out_plans_fd(int fd, const Battle *b, const Plan plan[], int side);
void out_field_fd(int fd, const Battle *b);
void out_field(const Battle *b);
const char *out_end(End end);
int out_stats(int fd, const Battle *b, End end);

#endif
