#ifndef TPEMOTE_H
#define TPEMOTE_H
/* LiveVita: third-party chat emotes (BetterTTV and FrankerFaceZ). Twitch's own emotes come through the IRC
 * "emotes" tag, but most emotes people type (BTTV / FFZ) are sent as plain words; this module fetches each
 * channel's BTTV + FFZ emote lists (PNG only, so the Vita can decode them) and maps a word to its image URL.
 * 7TV is left out because its images are WebP, which the Vita can't decode. Runs its own network thread. */
#include <stddef.h>

void tpemote_start(void);                       /* once: start the thread, load the global emotes */
void tpemote_set_channel(const char *broadcaster_id);   /* load this channel's emotes (by Twitch user id) */
/* if word is a known third-party emote, fills its image URL and returns 1 (UI thread) */
int  tpemote_lookup(const char *word, char *url, size_t cap);

#endif
