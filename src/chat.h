#ifndef CHAT_H
#define CHAT_H
/* LiveVita: Twitch chat over IRC (irc.chat.twitch.tv:6697, TLS) on its own thread.
 * - Reads anonymously (justinfan + a random number) until a signed-in user's token is given, then signs in
 *   so messages can be sent too.
 * - IRCv3 tags (twitch.tv/tags, twitch.tv/commands): name colours, badges, emotes, moderator clears.
 * - The last CHAT_MAX_MSGS messages are kept in a ring buffer; read them between chat_lock / chat_unlock. */
#include <stdint.h>

#define CHAT_MAX_MSGS   100
#define CHAT_TEXT_MAX   512
#define CHAT_MAX_EMOTES 16

enum {                                  /* ChatMsg.badges */
    CB_BROADCASTER = 1, CB_MOD = 2, CB_VIP = 4, CB_SUB = 8, CB_PARTNER = 16, CB_STAFF = 32, CB_TURBO = 64
};
enum { CK_MSG, CK_ACTION, CK_SYSTEM, CK_OWN };   /* ChatMsg.kind */

typedef struct {
    uint16_t start, end;                /* bytes [start, end) of text */
    char id[28];                        /* emote id (static-cdn.jtvnw.net/emoticons/v2/<id>/...) */
} ChatEmote;

typedef struct {
    char time[6];                       /* "21:04" (local) */
    char name[40];                      /* display name ("" for system lines) */
    char user[40];                      /* login (for moderator clears) */
    char id[40];                        /* message id (for CLEARMSG) */
    uint32_t color;                     /* 0xRRGGBB, 0 = none given */
    uint8_t badges;
    uint8_t kind;
    uint8_t deleted;                    /* removed by a moderator */
    uint8_t nemotes;
    ChatEmote em[CHAT_MAX_EMOTES];
    char text[CHAT_TEXT_MAX];
} ChatMsg;

enum { CHAT_OFF, CHAT_CONNECTING, CHAT_JOINED, CHAT_WAITING };   /* chat_state() */

void chat_start(void);                  /* once: starts the thread */
void chat_set_enabled(int on);          /* off: disconnect and stay away */
/* the signed-in user (login + token with chat:edit), or NULL / "" to read anonymously */
void chat_set_auth(const char *login, const char *token);
void chat_join(const char *channel);    /* a channel login; clears the messages */
void chat_leave(void);
/* queues a message to the joined channel: 0 ok, -1 not signed in, -2 not in a channel, -3 too fast */
int  chat_send(const char *text);
int  chat_state(void);
int  chat_signed_in(void);              /* connected with the user's token (can send) */

void chat_lock(void);
void chat_unlock(void);
int  chat_count(void);                  /* (while locked) messages kept */
const ChatMsg *chat_msg(int i);         /* (while locked) 0 = oldest */
uint32_t chat_version(void);            /* changes whenever messages change */

/* for tests on the PC: one line from the server (without CR LF) */
void chat_test_line(const char *line);

#endif
