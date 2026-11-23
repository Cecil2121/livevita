#ifndef PLAYERUI_H
#define PLAYERUI_H
#include "ui.h"
/* LiveVita: the player screen (theater mode with chat, or full screen) */
void player_screen_draw(void);
void player_screen_input(const Input *in);
/* the sign-in / follow dialogs (also over the handheld pages) */
void dialog_draw(void);
int  dialog_input(const Input *in);   /* 1 = the dialog took the input */
#endif
