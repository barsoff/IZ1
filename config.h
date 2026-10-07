#ifndef CONFIG_H
#define CONFIG_H

#include "battle.h"

void cfg_default(Cfg *cfg, Battle *b);
int cfg_load(const char *path, Cfg *cfg, Battle *b);
int num(const char *s, int lo, int hi, int *val);

#endif
