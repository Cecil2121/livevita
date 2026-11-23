#ifndef FAVCAT_H
#define FAVCAT_H
/* LiveVita: categories followed on this Vita (ux0:data/LiveVita/categories.txt). Twitch has no public way for
 * apps to follow categories on an account, so they are kept here and shown first in Games. */
#include "twitch.h"

#define FAVCAT_MAX 50

void favcat_load(void);
int  favcat_count(void);
const TwGame *favcat_get(int i);
int  favcat_has(const char *id);
/* follows or unfollows a category; returns 1 if it is followed now */
int  favcat_toggle(const TwGame *g);
/* the games to show: the followed ones first (marked), then the others from top (in their order) */
int  favcat_merge(const TwGame *top, int ntop, TwGame *out, int max);

#endif
