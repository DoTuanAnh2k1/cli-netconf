/*
 * log.h — Lightweight, macro-based logging for CLI NETCONF.
 *
 * Features:
 *   - Zero overhead khi level bị tắt (chỉ 1 phép so sánh int)
 *   - Log ra file riêng (không lẫn stdout/stderr của CLI)
 *   - Tự động ghi timestamp, level, source file:line
 *   - Cấu hình qua env: LOG_LEVEL (debug|info|warn|error|off), LOG_FILE
 *
 * Usage:
 *   #include "log.h"
 *
 *   // Gọi 1 lần trong main():
 *   log_init();
 *
 *   // Sau đó dùng:
 *   LOG_DEBUG("connecting to %s:%d", host, port);
 *   LOG_INFO("user %s logged in", username);
 *   LOG_WARN("NE %s unreachable", ne_name);
 *   LOG_ERROR("MAAPI dial failed: %s", strerror(errno));
 *
 *   // Khi thoát:
 *   log_close();
 */
#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>

/* ── Log levels ─────────────────────────────────────────── */
enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO  = 1,
    LOG_LVL_WARN  = 2,
    LOG_LVL_ERROR = 3,
    LOG_LVL_OFF   = 4
};

/* ── Global state (internal) ────────────────────────────── */
static FILE *g_log_fp    = NULL;
static int   g_log_level = LOG_LVL_INFO;  /* Mặc định: INFO */

/* ── Tên level cho output ───────────────────────────────── */
static inline const char *log_level_str(int level) {
    switch (level) {
        case LOG_LVL_DEBUG: return "DEBUG";
        case LOG_LVL_INFO:  return "INFO ";
        case LOG_LVL_WARN:  return "WARN ";
        case LOG_LVL_ERROR: return "ERROR";
        default:            return "?????";
    }
}

/* ── Parse level từ string (env var) ────────────────────── */
static inline int log_parse_level(const char *s) {
    if (!s || !*s)                          return LOG_LVL_INFO;
    if (strcasecmp(s, "debug") == 0)        return LOG_LVL_DEBUG;
    if (strcasecmp(s, "info")  == 0)        return LOG_LVL_INFO;
    if (strcasecmp(s, "warn")  == 0)        return LOG_LVL_WARN;
    if (strcasecmp(s, "warning") == 0)      return LOG_LVL_WARN;
    if (strcasecmp(s, "error") == 0)        return LOG_LVL_ERROR;
    if (strcasecmp(s, "err")   == 0)        return LOG_LVL_ERROR;
    if (strcasecmp(s, "off")   == 0)        return LOG_LVL_OFF;
    if (strcasecmp(s, "none")  == 0)        return LOG_LVL_OFF;
    return LOG_LVL_INFO;
}

/*
 * log_init — Khởi tạo logger. Gọi 1 lần trong main().
 *
 * Env vars:
 *   LOG_LEVEL  = debug | info | warn | error | off   (default: info)
 *   LOG_FILE   = path to log file                     (default: /tmp/cli-netconf.log)
 */
static inline void log_init(void) {
    g_log_level = log_parse_level(getenv("LOG_LEVEL"));
    if (g_log_level >= LOG_LVL_OFF) return;  /* Logging tắt hoàn toàn */

    const char *path = getenv("LOG_FILE");
    if (!path || !*path) path = "/tmp/cli-netconf.log";

    g_log_fp = fopen(path, "a");
    if (!g_log_fp) {
        fprintf(stderr, "[log] cannot open %s: %s\n", path, strerror(errno));
        return;
    }
    /* Line-buffered: flush mỗi dòng, không mất log khi crash */
    setvbuf(g_log_fp, NULL, _IOLBF, 0);
}

/*
 * log_close — Đóng file log. Gọi trước khi exit.
 */
static inline void log_close(void) {
    if (g_log_fp) {
        fclose(g_log_fp);
        g_log_fp = NULL;
    }
}

/*
 * log_write — Ghi 1 dòng log. Không gọi trực tiếp — dùng macro LOG_*.
 *
 * Format: 2026-04-16 09:30:15 [INFO ] main.c:42  user admin logged in
 */
static inline void log_write(int level, const char *file, int line,
                              const char *fmt, ...) {
    if (!g_log_fp) return;

    /* Timestamp */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    /* Lấy tên file không có đường dẫn */
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

    fprintf(g_log_fp, "%s [%s] %s:%-4d  ", ts, log_level_str(level), base, line);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log_fp, fmt, ap);
    va_end(ap);

    fputc('\n', g_log_fp);
    /* Line-buffered → tự flush, nhưng explicit flush cho ERROR */
    if (level >= LOG_LVL_ERROR) fflush(g_log_fp);
}

/* ── Macro API — zero-cost khi level bị tắt ────────────── */

#define LOG_DEBUG(fmt, ...) \
    do { if (g_log_level <= LOG_LVL_DEBUG) \
        log_write(LOG_LVL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while (0)

#define LOG_INFO(fmt, ...) \
    do { if (g_log_level <= LOG_LVL_INFO) \
        log_write(LOG_LVL_INFO, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while (0)

#define LOG_WARN(fmt, ...) \
    do { if (g_log_level <= LOG_LVL_WARN) \
        log_write(LOG_LVL_WARN, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while (0)

#define LOG_ERROR(fmt, ...) \
    do { if (g_log_level <= LOG_LVL_ERROR) \
        log_write(LOG_LVL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__); \
    } while (0)

#endif /* LOG_H */
