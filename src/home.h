#ifndef HOME_H
#define HOME_H
#include "ui.h"
/* LiveVita: the home screens (lists, categories, search, settings) in the handheld or TV layout */
void home_draw(void);
void home_input(const Input *in);
/* the settings list (shared by both layouts); top = y of the first row */
void home_settings_draw(int tv, int top);
void home_settings_input(const Input *in, int top);
/* the handheld layout (hh.c) */
void hh_draw(void);
void hh_input(const Input *in);
/* the TV layout (tv.c), in the style of the current Twitch TV apps */
void tv_draw(void);
void tv_input(const Input *in);
#endif
