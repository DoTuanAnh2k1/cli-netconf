/*
 * log.c — Implementation của logging system.
 *
 * Globals ở đây được share across all compilation units qua extern trong log.h.
 * Trước đây globals là static-inline trong log.h nên mỗi .c có một copy
 * riêng → LOG_* trong maapi-ops.c không ghi vào cùng file với main.c. Refactor
 * này fix vấn đề đó.
 */
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

/* ── Runtime state ──────────────────────────────────────── */
int   g_log_level  = LOG_LVL_INFO;   /* Mặc định: INFO */
static FILE *g_log_fp     = NULL;
static int   g_log_stderr = 0;       /* 1 = song song ghi ra stderr */
static FILE *g_log_pid1   = NULL;    /* /proc/1/fd/2 — docker logs */

/* ── Helpers ────────────────────────────────────────────── */
static const char *log_level_str(int level) {
    switch (level) {
        case LOG_LVL_DEBUG: return "DEBUG";
        case LOG_LVL_INFO:  return "INFO ";
        case LOG_LVL_WARN:  return "WARN ";
        case LOG_LVL_ERROR: return "ERROR";
        default:            return "?????";
    }
}

static int log_parse_level(const char *s) {
    if (!s || !*s)                       return LOG_LVL_INFO;
    if (strcasecmp(s, "debug")   == 0)   return LOG_LVL_DEBUG;
    if (strcasecmp(s, "info")    == 0)   return LOG_LVL_INFO;
    if (strcasecmp(s, "warn")    == 0)   return LOG_LVL_WARN;
    if (strcasecmp(s, "warning") == 0)   return LOG_LVL_WARN;
    if (strcasecmp(s, "error")   == 0)   return LOG_LVL_ERROR;
    if (strcasecmp(s, "err")     == 0)   return LOG_LVL_ERROR;
    if (strcasecmp(s, "off")     == 0)   return LOG_LVL_OFF;
    if (strcasecmp(s, "none")    == 0)   return LOG_LVL_OFF;
    return LOG_LVL_INFO;
}

void load_env_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0 || line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;
        size_t vl = strlen(val);
        if (vl >= 2 && ((val[0] == '"' && val[vl-1] == '"') ||
                        (val[0] == '\'' && val[vl-1] == '\''))) {
            char *mv = (char *)val;
            mv[vl-1] = '\0';
            val = mv + 1;
        }
        setenv(key, val, 0);
    }
    fclose(fp);
}

void log_init(void) {
    g_log_level = log_parse_level(getenv("LOG_LEVEL"));
    if (g_log_level >= LOG_LVL_OFF) return;

    const char *path = getenv("LOG_FILE");
    if (!path || !*path) path = "/tmp/cli-netconf.log";

    g_log_fp = fopen(path, "a");
    if (!g_log_fp) {
        fprintf(stderr, "[log] cannot open %s: %s\n", path, strerror(errno));
    } else {
        setvbuf(g_log_fp, NULL, _IOLBF, 0);
    }

    /* LOG_STDERR: song song ghi ra stderr (terminal user). Mặc định TẮT khi
     * env unset — trước đây auto-bật theo isatty(stderr) làm noisy terminal
     * user khi có WARN/ERROR. Bật explicit nếu cần debug local. */
    const char *stderr_env = getenv("LOG_STDERR");
    if (stderr_env && *stderr_env)
        g_log_stderr = (stderr_env[0] != '0');
    else
        g_log_stderr = 0;

    /* LOG_PID1=1 → copy log sang stderr của PID 1 (sshd) để xuất hiện trong
     * `docker logs`. Dockerfile set env này. */
    const char *pid1_env = getenv("LOG_PID1");
    if (pid1_env && pid1_env[0] != '0') {
        g_log_pid1 = fopen("/proc/1/fd/2", "w");
        if (g_log_pid1) setvbuf(g_log_pid1, NULL, _IOLBF, 0);
    }
}

void log_close(void) {
    if (g_log_fp)   { fclose(g_log_fp);   g_log_fp   = NULL; }
    if (g_log_pid1) { fclose(g_log_pid1); g_log_pid1 = NULL; }
}

void log_write(int level, const char *file, int line,
               const char *fmt, ...) {
    if (!g_log_fp && !g_log_stderr && !g_log_pid1) return;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

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
        if (level >= LOG_LVL_WARN)
            fprintf(stderr, "[%s] %s:%d %s\n",
                    log_level_str(level), base, line, msg);
        else
            fprintf(stderr, "[%s] %s\n", log_level_str(level), msg);
        fflush(stderr);
    }
    if (g_log_pid1) {
        fprintf(g_log_pid1, "%s [cli] [%s] %s\n",
                ts, log_level_str(level), msg);
    }
}
