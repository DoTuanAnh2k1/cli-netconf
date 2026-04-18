/*
 * log.h — Lightweight, macro-based logging for CLI NETCONF.
 *
 * Sau refactor: header chỉ khai báo. Implementation + globals nằm trong
 * src/log.c. Nhờ vậy tất cả compilation unit chia sẻ cùng state (log level,
 * file handle) và việc LOG_* trong bất kỳ file nào cũng chạy đúng.
 *
 * Env điều khiển:
 *   LOG_LEVEL  = debug | info | warn | error | off   (default: info)
 *   LOG_FILE   = path to log file                    (default: /tmp/cli-netconf.log)
 *   LOG_STDERR = 0 | 1                               (default: auto theo tty)
 *   LOG_PID1   = 0 | 1                               (default: 0)
 *
 * Usage:
 *   log_init();                    // Gọi 1 lần trong main()
 *   LOG_INFO("user %s logged in", u);
 *   log_close();                   // Trước khi exit
 */
#ifndef LOG_H
#define LOG_H

#include <stdio.h>

/* ── Log levels ─────────────────────────────────────────── */
enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO  = 1,
    LOG_LVL_WARN  = 2,
    LOG_LVL_ERROR = 3,
    LOG_LVL_OFF   = 4
};

/* Current runtime level — macro dùng để skip work khi level bị tắt */
extern int g_log_level;

/* ── API ────────────────────────────────────────────────── */
void log_init(void);
void log_close(void);
void log_write(int level, const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/*
 * load_env_file — Nạp KEY=VALUE từ file vào môi trường process.
 * Dùng cho sshd child không kế thừa env từ container.
 * Không ghi đè env đã có sẵn. Silent nếu file không tồn tại.
 */
void load_env_file(const char *path);

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
