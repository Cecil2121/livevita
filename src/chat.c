#include "chat.h"
#include "applog.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#ifndef CHAT_NO_NET
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#endif
#ifdef __vita__
#include <psp2/rtc.h>
#include <psp2/kernel/processmgr.h>
#endif

#define IRC_HOST "irc.chat.twitch.tv"
#define IRC_PORT "6697"
#define SEND_QUEUE 4

/* ---------------------------------------------------------------- shared state (under g_mx) */

static pthread_mutex_t g_mx = PTHREAD_MUTEX_INITIALIZER;
static ChatMsg g_ring[CHAT_MAX_MSGS];
static int g_head, g_count;                     /* oldest at g_head */
static volatile uint32_t g_version;

static int g_enabled = 1;
static char g_want_channel[40];                 /* the channel the UI wants */
static char g_login[40], g_token[128];          /* "" = anonymous */
static int g_auth_gen;                          /* changes when the sign-in changes */
static char g_sendq[SEND_QUEUE][CHAT_TEXT_MAX];
static int g_nsend;
static volatile int g_state = CHAT_OFF, g_signed;
static unsigned g_sent_ms[20];                  /* times of the last 20 messages sent (Twitch: 20 per 30 s) */

/* own name / colour / badges for the messages we send (Twitch does not echo them) */
static char g_own_name[40];
static uint32_t g_own_color;
static uint8_t g_own_badges;

void chat_lock(void) { pthread_mutex_lock(&g_mx); }
void chat_unlock(void) { pthread_mutex_unlock(&g_mx); }
int chat_count(void) { return g_count; }
const ChatMsg *chat_msg(int i) { return &g_ring[(g_head + i) % CHAT_MAX_MSGS]; }
uint32_t chat_version(void) { return g_version; }
int chat_state(void) { return g_state; }
int chat_signed_in(void) { return g_signed; }

static unsigned now_ms(void)
{
#ifdef __vita__
    return (unsigned)(sceKernelGetProcessTimeWide() / 1000);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
#endif
}

static void hhmm(char out[6])
{
#ifdef __vita__
    SceDateTime t;
    if (sceRtcGetCurrentClockLocalTime(&t) >= 0) { snprintf(out, 6, "%02u:%02u", t.hour % 24, t.minute % 60); return; }
    snprintf(out, 6, "--:--");
#else
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    snprintf(out, 6, "%02d:%02d", tm ? tm->tm_hour : 0, tm ? tm->tm_min : 0);
#endif
}

/* a new message slot (the oldest is dropped when full); call locked */
static ChatMsg *ring_add(void)
{
    ChatMsg *m;
    if (g_count < CHAT_MAX_MSGS) m = &g_ring[(g_head + g_count++) % CHAT_MAX_MSGS];
    else { m = &g_ring[g_head]; g_head = (g_head + 1) % CHAT_MAX_MSGS; }
    memset(m, 0, sizeof *m);
    hhmm(m->time);
    g_version++;
    return m;
}

static void ring_clear(void)
{
    g_head = g_count = 0;
    g_version++;
}

static void add_system(const char *text)
{
    chat_lock();
    ChatMsg *m = ring_add();
    m->kind = CK_SYSTEM;
    snprintf(m->text, sizeof m->text, "%s", text);
    chat_unlock();
}

/* ---------------------------------------------------------------- UI side */

void chat_set_enabled(int on)
{
    chat_lock();
    g_enabled = on;
    chat_unlock();
}

void chat_set_auth(const char *login, const char *token)
{
    chat_lock();
    const char *l = login && token && token[0] ? login : "", *t = login && token && token[0] ? token : "";
    if (strcmp(l, g_login) || strcmp(t, g_token)) {
        snprintf(g_login, sizeof g_login, "%s", l);
        snprintf(g_token, sizeof g_token, "%s", t);
        g_auth_gen++;
    }
    chat_unlock();
}

void chat_join(const char *channel)
{
    chat_lock();
    if (strcmp(channel, g_want_channel)) {
        snprintf(g_want_channel, sizeof g_want_channel, "%s", channel);
        for (char *p = g_want_channel; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        ring_clear();
        g_nsend = 0;
    }
    chat_unlock();
}

void chat_leave(void) { chat_join(""); }

int chat_send(const char *text)
{
    int r = 0;
    chat_lock();
    unsigned now = now_ms();
    int recent = 0;
    for (int i = 0; i < 20; i++) if (g_sent_ms[i] && now - g_sent_ms[i] < 30000) recent++;
    if (!g_signed) r = -1;
    else if (!g_want_channel[0] || g_state != CHAT_JOINED) r = -2;
    else if (g_nsend >= SEND_QUEUE || recent >= 19) r = -3;
    else {
        snprintf(g_sendq[g_nsend++], CHAT_TEXT_MAX, "%s", text);
        memmove(g_sent_ms, g_sent_ms + 1, sizeof g_sent_ms - sizeof g_sent_ms[0]);
        g_sent_ms[19] = now;
    }
    chat_unlock();
    return r;
}

/* ---------------------------------------------------------------- IRC line parsing */

/* tag value with IRCv3 escapes decoded */
static void tag_get(const char *tags, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    if (!tags) return;
    size_t kl = strlen(key);
    const char *p = tags;
    while (*p) {
        const char *e = p + strcspn(p, ";");
        const char *eq = memchr(p, '=', (size_t)(e - p));
        if (eq && (size_t)(eq - p) == kl && !strncmp(p, key, kl)) {
            size_t o = 0;
            for (const char *v = eq + 1; v < e && o + 1 < cap; v++) {
                if (*v == '\\' && v + 1 < e) {
                    v++;
                    char c = *v == 's' ? ' ' : *v == ':' ? ';' : *v == 'r' ? '\r' : *v == 'n' ? '\n' : *v;
                    out[o++] = c;
                } else out[o++] = *v;
            }
            out[o] = 0;
            return;
        }
        if (!*e) break;
        p = e + 1;
    }
}

static uint32_t parse_color(const char *s)
{
    if (s[0] != '#' || strlen(s) < 7) return 0;
    return (uint32_t)strtoul(s + 1, NULL, 16) & 0xFFFFFF;
}

static uint8_t parse_badges(const char *b)
{
    uint8_t f = 0;
    if (strstr(b, "broadcaster/")) f |= CB_BROADCASTER;
    if (strstr(b, "moderator/")) f |= CB_MOD;
    if (strstr(b, "vip/")) f |= CB_VIP;
    if (strstr(b, "subscriber/") || strstr(b, "founder/")) f |= CB_SUB;
    if (strstr(b, "partner/")) f |= CB_PARTNER;
    if (strstr(b, "staff/") || strstr(b, "admin/") || strstr(b, "global_mod/")) f |= CB_STAFF;
    if (strstr(b, "turbo/") || strstr(b, "premium/")) f |= CB_TURBO;
    return f;
}

/* emotes tag "id:a-b,c-d/id2:e-f" (positions in characters) -> byte ranges in text */
static void parse_emotes(ChatMsg *m, const char *tag)
{
    /* byte offset of each character (code point) */
    static uint16_t cp_off[CHAT_TEXT_MAX + 1];
    int ncp = 0;
    size_t len = strlen(m->text);
    for (size_t i = 0; i < len; i++) if (((unsigned char)m->text[i] & 0xC0) != 0x80) cp_off[ncp++] = (uint16_t)i;
    cp_off[ncp] = (uint16_t)len;
    const char *p = tag;
    while (*p && m->nemotes < CHAT_MAX_EMOTES) {
        const char *colon = strchr(p, ':');
        if (!colon) break;
        char id[28];
        size_t il = (size_t)(colon - p) < sizeof id - 1 ? (size_t)(colon - p) : sizeof id - 1;
        memcpy(id, p, il);
        id[il] = 0;
        p = colon + 1;
        while (*p && *p != '/' && m->nemotes < CHAT_MAX_EMOTES) {
            int a = atoi(p);
            const char *dash = strchr(p, '-');
            if (!dash) break;
            int b = atoi(dash + 1);
            if (a >= 0 && b >= a && b < ncp) {
                ChatEmote *e = &m->em[m->nemotes++];
                e->start = cp_off[a];
                e->end = cp_off[b + 1];
                snprintf(e->id, sizeof e->id, "%s", id);
            }
            p = dash + 1;
            while (*p && *p != ',' && *p != '/') p++;
            if (*p == ',') p++;
        }
        if (*p == '/') p++;
    }
    /* in text order (drawn left to right) */
    for (int i = 1; i < m->nemotes; i++)
        for (int k = i; k > 0 && m->em[k].start < m->em[k - 1].start; k--) {
            ChatEmote t = m->em[k]; m->em[k] = m->em[k - 1]; m->em[k - 1] = t;
        }
}

/* control characters become spaces (the font has no glyph for them) */
static void clean_text(char *s)
{
    for (; *s; s++) if ((unsigned char)*s < 32) *s = ' ';
}

static void add_user_msg(const char *tags, const char *login, const char *text)
{
    char v[200];
    chat_lock();
    ChatMsg *m = ring_add();
    m->kind = CK_MSG;
    snprintf(m->user, sizeof m->user, "%s", login);
    tag_get(tags, "display-name", m->name, sizeof m->name);
    if (!m->name[0]) snprintf(m->name, sizeof m->name, "%s", login);
    tag_get(tags, "id", m->id, sizeof m->id);
    tag_get(tags, "color", v, sizeof v);
    m->color = parse_color(v);
    tag_get(tags, "badges", v, sizeof v);
    m->badges = parse_badges(v);
    size_t tl = strlen(text);
    if (tl > 8 && !strncmp(text, "\001ACTION ", 8)) {            /* /me */
        m->kind = CK_ACTION;
        text += 8;
        tl -= 8;
        if (tl && text[tl - 1] == '\001') tl--;
    }
    if (tl >= sizeof m->text) tl = sizeof m->text - 1;
    while (tl && ((unsigned char)text[tl] & 0xC0) == 0x80) tl--;  /* (not inside a character) */
    memcpy(m->text, text, tl);
    m->text[tl] = 0;
    char *et = malloc(2048);
    if (et) { tag_get(tags, "emotes", et, 2048); parse_emotes(m, et); free(et); }
    clean_text(m->text);
    chat_unlock();
}

/* what the thread must do after a line */
enum { LINE_OK, LINE_PONG, LINE_JOINED, LINE_AUTH_FAILED, LINE_RECONNECT };

static int handle_line(char *line, char *pong, size_t pcap)
{
    char *tags = NULL, *prefix = NULL, *cmd, *params, *trail = NULL;
    if (*line == '@') { tags = line + 1; char *s = strchr(line, ' '); if (!s) return LINE_OK; *s = 0; line = s + 1; }
    if (*line == ':') { prefix = line + 1; char *s = strchr(line, ' '); if (!s) return LINE_OK; *s = 0; line = s + 1; }
    cmd = line;
    char *s = strchr(line, ' ');
    params = s ? s + 1 : (char *)"";
    if (s) *s = 0;
    char *t = strstr(params, " :");
    if (t) { *t = 0; trail = t + 2; }
    else if (*params == ':') { trail = params + 1; params = (char *)""; }
    char login[40] = "";
    if (prefix) {
        size_t n = strcspn(prefix, "!@");
        if (n >= sizeof login) n = sizeof login - 1;
        memcpy(login, prefix, n);
        login[n] = 0;
    }

    if (!strcmp(cmd, "PING")) { snprintf(pong, pcap, "PONG :%s\r\n", trail ? trail : "tmi.twitch.tv"); return LINE_PONG; }
    if (!strcmp(cmd, "RECONNECT")) return LINE_RECONNECT;
    if (!strcmp(cmd, "PRIVMSG") && trail) { add_user_msg(tags, login, trail); return LINE_OK; }
    if (!strcmp(cmd, "JOIN")) return LINE_JOINED;
    if (!strcmp(cmd, "USERNOTICE")) {                           /* subs, raids, gifts... */
        char sys[400];
        tag_get(tags, "system-msg", sys, sizeof sys);
        if (sys[0]) { clean_text(sys); add_system(sys); }
        if (trail && *trail) {
            char l[40];
            tag_get(tags, "login", l, sizeof l);
            add_user_msg(tags, l[0] ? l : login, trail);
        }
        return LINE_OK;
    }
    if (!strcmp(cmd, "CLEARCHAT")) {
        chat_lock();
        if (trail && *trail) {                                  /* timeout / ban of one user */
            for (int i = 0; i < g_count; i++) {
                ChatMsg *m = (ChatMsg *)chat_msg(i);
                if (m->kind != CK_SYSTEM && !strcasecmp(m->user, trail)) m->deleted = 1;
            }
            g_version++;
            chat_unlock();
        } else {
            ring_clear();
            chat_unlock();
            add_system("Chat was cleared by a moderator.");
        }
        return LINE_OK;
    }
    if (!strcmp(cmd, "CLEARMSG")) {
        char id[40];
        tag_get(tags, "target-msg-id", id, sizeof id);
        chat_lock();
        for (int i = 0; i < g_count && id[0]; i++) {
            ChatMsg *m = (ChatMsg *)chat_msg(i);
            if (!strcmp(m->id, id)) m->deleted = 1;
        }
        g_version++;
        chat_unlock();
        return LINE_OK;
    }
    if (!strcmp(cmd, "GLOBALUSERSTATE") || !strcmp(cmd, "USERSTATE")) {
        char v[200];
        chat_lock();
        tag_get(tags, "display-name", v, sizeof v);
        if (v[0]) snprintf(g_own_name, sizeof g_own_name, "%s", v);
        tag_get(tags, "color", v, sizeof v);
        if (v[0]) g_own_color = parse_color(v);
        tag_get(tags, "badges", v, sizeof v);
        g_own_badges = parse_badges(v);
        chat_unlock();
        return LINE_OK;
    }
    if (!strcmp(cmd, "NOTICE") && trail) {
        if (strstr(trail, "Login authentication failed") || strstr(trail, "Improperly formatted auth")) return LINE_AUTH_FAILED;
        char msg[CHAT_TEXT_MAX];
        snprintf(msg, sizeof msg, "%s", trail);
        clean_text(msg);
        add_system(msg);
        return LINE_OK;
    }
    return LINE_OK;
}

void chat_test_line(const char *line)
{
    char buf[4096], pong[128];
    snprintf(buf, sizeof buf, "%s", line);
    int r = handle_line(buf, pong, sizeof pong);
    if (r == LINE_PONG) printf("[would send] %s", pong);
    else if (r != LINE_OK) printf("[line result %d]\n", r);
}

/* ---------------------------------------------------------------- connection (TLS) */

#ifndef CHAT_NO_NET
typedef struct {
    int fd;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_config conf;
    mbedtls_ssl_context ssl;
    int tls_up;
} Conn;

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    Conn *c = ctx;
    ssize_t n = send(c->fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    Conn *c = ctx;
    struct pollfd p = { c->fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout ? (int)timeout : 200);
    if (r == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (r < 0) return errno == EINTR ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    ssize_t n = recv(c->fd, buf, len, 0);
    if (n < 0) return (errno == EAGAIN || errno == EINTR) ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;                                              /* 0 = closed */
}

static void conn_close(Conn *c)
{
    if (c->tls_up) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->ent);
        c->tls_up = 0;
    }
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

/* TCP connect with a 10 s limit, then the TLS handshake. 0 = ok. */
static int conn_open(Conn *c)
{
    memset(c, 0, sizeof *c);
    c->fd = -1;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(IRC_HOST, IRC_PORT, &hints, &res) != 0 || !res) { plog("chat: cannot resolve %s", IRC_HOST); return -1; }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY) { close(fd); plog("chat: connect %d", errno); return -1; }
    if (r < 0) {
        struct pollfd p = { fd, POLLOUT, 0 };
        int e = 0;
        socklen_t el = sizeof e;
        if (poll(&p, 1, 10000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) < 0 || e) {
            close(fd);
            plog("chat: connect failed (%d)", e);
            return -1;
        }
    }
    fcntl(fd, F_SETFL, fl);
    c->fd = fd;
    mbedtls_entropy_init(&c->ent);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ssl_init(&c->ssl);
    c->tls_up = 1;
    r = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent, (const unsigned char *)"LiveVitaChat", 12);
    if (!r) r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (!r) {
        /* (like the rest of the app: the Vita has no current root certificates, so they are not checked) */
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
        mbedtls_ssl_conf_read_timeout(&c->conf, 200);
        r = mbedtls_ssl_setup(&c->ssl, &c->conf);
    }
    if (!r) r = mbedtls_ssl_set_hostname(&c->ssl, IRC_HOST);
    if (r) { plog("chat: TLS setup -0x%04X", (unsigned)-r); conn_close(c); return -1; }
    mbedtls_ssl_set_bio(&c->ssl, c, bio_send, NULL, bio_recv_timeout);
    unsigned t0 = now_ms();
    while ((r = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if ((r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_TIMEOUT) && now_ms() - t0 < 15000) continue;
        plog("chat: TLS handshake -0x%04X", (unsigned)-r);
        conn_close(c);
        return -1;
    }
    return 0;
}

static int conn_write(Conn *c, const char *s)
{
    size_t len = strlen(s), off = 0;
    while (off < len) {
        int n = mbedtls_ssl_write(&c->ssl, (const unsigned char *)s + off, len - off);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

/* > 0 bytes, 0 = nothing for now, < 0 closed / error */
static int conn_read(Conn *c, char *buf, size_t len)
{
    int n = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len);
    if (n > 0) return n;
    if (n == MBEDTLS_ERR_SSL_TIMEOUT || n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
    if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return 0;
#endif
    return -1;
}

/* ---------------------------------------------------------------- the thread */

static void set_state(int s) { g_state = s; }

/* one connection: returns when it ends (0 = asked to stop / changed, -1 = failed) */
static int session(void)
{
    char login[40], token[128], line[8192 + 1], wbuf[CHAT_TEXT_MAX + 80], pong[128];
    int auth_gen;
    chat_lock();
    snprintf(login, sizeof login, "%s", g_login);
    snprintf(token, sizeof token, "%s", g_token);
    auth_gen = g_auth_gen;
    chat_unlock();
    static int anon_after_fail;                                 /* the token was refused: read anonymously */
    if (anon_after_fail == auth_gen + 1) { login[0] = token[0] = 0; }

    set_state(CHAT_CONNECTING);
    Conn c;
    if (conn_open(&c) < 0) return -1;
    conn_write(&c, "CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");
    if (token[0] && login[0]) {
        snprintf(wbuf, sizeof wbuf, "PASS oauth:%s\r\nNICK %s\r\n", token, login);
    } else {
        snprintf(wbuf, sizeof wbuf, "NICK justinfan%d\r\n", 10000 + rand() % 80000);
    }
    if (conn_write(&c, wbuf) < 0) { conn_close(&c); return -1; }
    g_signed = token[0] && login[0];
    plog("chat: connected (%s)", g_signed ? "signed in" : "anonymous");

    char joined[40] = "";
    size_t have = 0;
    unsigned last_rx = now_ms(), ping_sent = 0;
    int ret = -1;
    for (;;) {
        /* what the UI wants now */
        char want[40], out[SEND_QUEUE][CHAT_TEXT_MAX];
        int nout = 0, enabled, gen;
        chat_lock();
        snprintf(want, sizeof want, "%s", g_want_channel);
        enabled = g_enabled;
        gen = g_auth_gen;
        if (!strcmp(want, joined) && joined[0]) { nout = g_nsend; memcpy(out, g_sendq, sizeof out[0] * (size_t)nout); g_nsend = 0; }
        chat_unlock();
        if (!enabled || !want[0] || gen != auth_gen) { ret = 0; break; }
        if (strcmp(want, joined)) {
            if (joined[0]) { snprintf(wbuf, sizeof wbuf, "PART #%s\r\n", joined); conn_write(&c, wbuf); }
            snprintf(wbuf, sizeof wbuf, "JOIN #%s\r\n", want);
            if (conn_write(&c, wbuf) < 0) break;
            snprintf(joined, sizeof joined, "%s", want);
            set_state(CHAT_CONNECTING);
        }
        for (int i = 0; i < nout; i++) {
            snprintf(wbuf, sizeof wbuf, "PRIVMSG #%s :%s\r\n", joined, out[i]);
            if (conn_write(&c, wbuf) < 0) break;
            chat_lock();                                        /* shown at once: Twitch does not send it back */
            ChatMsg *m = ring_add();
            m->kind = CK_OWN;
            snprintf(m->name, sizeof m->name, "%s", g_own_name[0] ? g_own_name : login);
            snprintf(m->user, sizeof m->user, "%s", login);
            m->color = g_own_color;
            m->badges = g_own_badges;
            snprintf(m->text, sizeof m->text, "%s", out[i]);
            clean_text(m->text);
            chat_unlock();
        }
        /* read */
        int n = conn_read(&c, line + have, sizeof line - 1 - have);
        if (n < 0) { plog("chat: connection closed"); break; }
        if (n > 0) {
            last_rx = now_ms();
            ping_sent = 0;
            have += (size_t)n;
            line[have] = 0;
            char *p = line, *eol;
            int stop = 0;
            while ((eol = strstr(p, "\r\n")) != NULL) {
                *eol = 0;
                int r = handle_line(p, pong, sizeof pong);
                if (r == LINE_PONG) conn_write(&c, pong);
                else if (r == LINE_JOINED && g_state != CHAT_JOINED) {
                    set_state(CHAT_JOINED);
                    char msg[120];
                    snprintf(msg, sizeof msg, "Connected to %s's chat%s", joined, g_signed ? "." : " (reading only: sign in to send messages).");
                    add_system(msg);
                } else if (r == LINE_AUTH_FAILED) {
                    add_system("Chat sign-in was refused: reading anonymously. Sign out and in again to send messages.");
                    anon_after_fail = auth_gen + 1;
                    stop = 1;
                    break;
                } else if (r == LINE_RECONNECT) { stop = 1; break; }
                p = eol + 2;
            }
            if (stop) { ret = 0; break; }
            have = strlen(p);
            memmove(line, p, have + 1);
            if (have >= sizeof line - 1) have = 0;                /* a line too long: dropped */
        } else {
            unsigned idle = now_ms() - last_rx;
            if (idle > 360000 && !ping_sent) { conn_write(&c, "PING :tmi.twitch.tv\r\n"); ping_sent = now_ms(); }
            if (ping_sent && now_ms() - ping_sent > 20000) { plog("chat: no answer to PING"); break; }
        }
    }
    conn_close(&c);
    g_signed = 0;
    return ret;
}

static void *chat_thread(void *arg)
{
    (void)arg;
    int backoff = 2000;
    for (;;) {
        char want[40];
        int enabled;
        chat_lock();
        snprintf(want, sizeof want, "%s", g_want_channel);
        enabled = g_enabled;
        chat_unlock();
        if (!enabled || !want[0]) {
            set_state(CHAT_OFF);
            usleep(150000);
            continue;
        }
        int r = session();
        if (r == 0) { backoff = 2000; continue; }               /* changed on purpose: straight on */
        set_state(CHAT_WAITING);
        chat_lock();
        int still = g_enabled && g_want_channel[0];
        chat_unlock();
        if (still) {
            char msg[100];
            snprintf(msg, sizeof msg, "Chat disconnected. Trying again in %d s...", backoff / 1000);
            add_system(msg);
        }
        for (int waited = 0; waited < backoff; waited += 150) {
            usleep(150000);
            chat_lock();
            int changed = strcmp(want, g_want_channel) != 0 || !g_enabled;
            chat_unlock();
            if (changed) break;
        }
        if (backoff < 30000) backoff *= 2;
    }
    return NULL;
}

void chat_start(void)
{
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 128 * 1024);
    pthread_create(&th, &at, chat_thread, NULL);
}
#else
void chat_start(void) {}
#endif
