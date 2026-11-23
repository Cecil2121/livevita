#ifndef APPLOG_H
#define APPLOG_H

#define DATA_DIR "ux0:data/LiveVita"
#define LOG_PATH DATA_DIR "/log.txt"

/* Appends one line to the log (also used by the player engine). */
void plog(const char *fmt, ...);
/* Keeps the last run's log as log_prev.txt and starts a new one. */
void app_log_reset(void);

#endif
