#ifndef FAVCHAN_H
#define FAVCHAN_H
/* LiveVita: channels followed on this Vita (ux0:data/LiveVita/channels.txt). Twitch removed the API for apps to
 * follow channels on an account (2021), so these are kept on the Vita and shown in the Following lists beside
 * the account's own follows. Safe to call from any thread. */
#include "twitch.h"

#define FAVCHAN_MAX 100

void favchan_load(void);
int  favchan_count(void);
/* a copy of entry i, or NULL (UI thread) */
const TwStream *favchan_at(int i);
int  favchan_has(const char *user_id);
/* follows / unfollows (needs user_id, login, name; avatar optional); returns 1 if followed now */
int  favchan_toggle(const TwStream *s);
/* a copy of the list (for the network thread) */
int  favchan_snapshot(TwStream *out, int max);

#endif
