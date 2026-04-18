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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

/* ── Log levels ─────────────────────────────────────────── */
enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO  = 1,
    LOG_LVL_WARN  = 2,
    LOG_LVL_ERROR = 3,
    LOG_LVL_OFF   = 4
};

/* ── Global state (internal) ────────────────────────────── */
static FILE *g_log_fp     = NULL;
static int   g_log_level  = LOG_LVL_INFO;  /* Mặc định: INFO */
static int   g_log_stderr = 0;             /* 1 = song song ghi ra stderr (terminal) */

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
 * load_env_file — Nạp KEY=VALUE từ file vào môi trường process.
 *
 * Dùng khi process không kế thừa env từ parent (ví dụ: sshd child không
 * có env container). App tự đọc file này ở startup để có config runtime.
 *
 * Format file: mỗi dòng KEY=VALUE. Bỏ qua dòng rỗng & dòng bắt đầu bằng '#'.
 * Không ghi đè env đã có sẵn (env passed in vẫn ưu tiên).
 * Silent nếu file không tồn tại (optional config).
 */
static inline void load_env_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* strip newline */
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        /* strip surrounding quotes on value */
        size_t vl = strlen(val);
        if (vl >= 2 && ((val[0] == '"' && val[vl-1] == '"') ||
                        (val[0] == '\'' && val[vl-1] == '\''))) {
            char *mv = (char *)val;
            mv[vl-1] = '\0';
            val = mv + 1;
        }
        setenv(key, val, 0);  /* 0 = không ghi đè env đã có */
    }
    fclose(fp);
}

/*
 * log_init — Khởi tạo logger. Gọi 1 lần trong main().
 *
 * Env vars:
 *   LOG_LEVEL  = debug | info | warn | error | off   (default: info)
 *   LOG_FILE   = path to log file                     (default: /tmp/cli-netconf.log)
 *   LOG_STDERR = 0 | 1                                (default: 1 nếu stderr là tty)
 */
static inline void log_init(void) {
    g_log_level = log_parse_level(getenv("LOG_LEVEL"));
    if (g_log_level >= LOG_LVL_OFF) return;  /* Logging tắt hoàn toàn */

    const char *path = getenv("LOG_FILE");
    if (!path || !*path) path = "/tmp/cli-netconf.log";

    g_log_fp = fopen(path, "a");
    if (!g_log_fp) {
        fprintf(stderr, "[log] cannot open %s: %s\n", path, strerror(errno));
    } else {
        /* Line-buffered: flush mỗi dòng, không mất log khi crash */
        setvbuf(g_log_fp, NULL, _IOLBF, 0);
    }

    /* Quyết định có ghi log ra terminal hay không */
    const char *stderr_env = getenv("LOG_STDERR");
    if (stderr_env && *stderr_env) {
        g_log_stderr = (stderr_env[0] != '0');
    } else {
        g_log_stderr = isatty(fileno(stderr)) ? 1 : 0;
    }
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
    if (!g_log_fp && !g_log_stderr) return;

    /* Timestamp */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    /* Lấy tên file không có đường dẫn */
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

    /* Format vào buffer 1 lần, ghi ra nhiều đích */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (g_log_fp) {
        fprintf(g_log_fp, "%s [%s] %s:%-4d  %s\n",
                ts, log_level_str(level), base, line, msg);
        if (level >= LOG_LVL_ERROR) fflush(g_log_fp);
    }
    if (g_log_stderr) {
        /* Ra terminal: gọn hơn, không hiện file:line cho INFO */
        if (level >= LOG_LVL_WARN)
            fprintf(stderr, "[%s] %s:%d %s\n",
                    log_level_str(level), base, line, msg);
        else
            fprintf(stderr, "[%s] %s\n", log_level_str(level), msg);
        fflush(stderr);
    }
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
