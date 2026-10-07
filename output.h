#ifndef OUTPUT_H
#define OUTPUT_H

#include "battle.h"

int out_open(const char *path, const char *src);
void out_field(const Battle *b);
int out_stats(int fd, const Battle *b, End end);

#endif
