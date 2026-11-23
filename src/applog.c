/* LiveVita: the log file (ux0:data/LiveVita/log.txt). The player engine writes to it through plog(). */
#include "applog.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_LIMIT (2u << 20)            /* stop writing past 2 MB (the app may run for hours) */
static volatile unsigned g_log_bytes;

void plog(const char *fmt, ...)
{
    if (g_log_bytes >= LOG_LIMIT) return;
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    g_log_bytes += (n > 0 ? (unsigned)n : 0) + 1;
    if (g_log_bytes >= LOG_LIMIT) fputs("log: 2 MB reached, nothing more is written until the app restarts\n", f);
    fclose(f);
}

void app_log_reset(void)
{
    remove(DATA_DIR "/log_prev.txt");
    rename(LOG_PATH, DATA_DIR "/log_prev.txt");
    FILE *f = fopen(LOG_PATH, "w");
    if (f) { fputs("LiveVita log\n", f); fclose(f); }
    g_log_bytes = 13;
}
