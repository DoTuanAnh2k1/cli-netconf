/*
 * main-direct.c — Direct mode: kết nối thẳng vào NE, bỏ qua mgt-service.
 *
 * Tương đương Go cmd/direct — không cần auth, không chọn NE,
 * vào thẳng command loop.
 *
 * Cấu hình qua env vars:
 *   NETCONF_HOST   host NE          (default: 127.0.0.1)
 *   NETCONF_PORT   port NE          (default: 2023)
 *   NETCONF_MODE   tcp | ssh        (default: tcp)
 *   NETCONF_USER   SSH username     (default: admin)
 *   NETCONF_PASS   SSH password     (default: admin)
 *   NE_NAME        label in prompt  (default: confd)
 *
 * Build:
 *   make -C c direct
 *   ./cli-netconf-c-direct
 *
 * Chạy với mock:
 *   make -C c run-direct
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "cli.h"

/* ── Helpers ──────────────────────────────── */

static const char *env_or(const char *key, const char *fallback) {
    const char *v = getenv(key);
    return (v && *v) ? v : fallback;
}

static int env_int_or(const char *key, int fallback) {
    const char *v = getenv(key);
    return (v && *v) ? atoi(v) : fallback;
}

/* ── Global session ───────────────────────── */
static cli_session_t g_session;

static void sigint_handler(int sig) {
    (void)sig;
    printf("\n");
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

static void update_prompt(cli_session_t *s) {
    if (s->current_ne) {
        snprintf(s->prompt, sizeof(s->prompt),
                 "%sdirect%s[%s%s%s]> ",
                 COLOR_CYAN, COLOR_RESET,
                 COLOR_YELLOW, s->current_ne->name, COLOR_RESET);
    } else {
        snprintf(s->prompt, sizeof(s->prompt),
                 "%sdirect%s> ", COLOR_CYAN, COLOR_RESET);
    }
}

/* ── command_loop ─────────────────────────── */
static void command_loop(cli_session_t *s) {
    while (1) {
        char *line = readline(s->prompt);
        if (!line) {           /* Ctrl+D → thoát */
            printf("\n");
            break;
        }

        char *trimmed = str_trim(line);
        if (!*trimmed) { free(line); continue; }

        add_history(trimmed);

        int argc = 0;
        char **argv = str_split(trimmed, &argc);

        if (argc > 0) {
            if (strcasecmp(argv[0], "exit") == 0 ||
                strcasecmp(argv[0], "quit") == 0) {
                free_tokens(argv, argc);
                free(line);
                break;
            } else {
                cmd_dispatch(s, trimmed);
            }
        }

        free_tokens(argv, argc);
        free(line);
    }
}

/* ── main ─────────────────────────────────── */
int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    xmlInitParser();
    signal(SIGINT, sigint_handler);
    rl_catch_signals = 0;

    /* Đọc config từ env */
    const char *host    = env_or("NETCONF_HOST", "127.0.0.1");
    int         port    = env_int_or("NETCONF_PORT", 2023);
    const char *mode    = env_or("NETCONF_MODE",  "tcp");
    const char *ne_name = env_or("NE_NAME",       "confd");
#ifdef WITH_SSH
    const char *user    = env_or("NETCONF_USER",  "admin");
    const char *pass    = env_or("NETCONF_PASS",  "admin");
#endif

    printf("%s", COLOR_GREEN);
    printf("============================================\n");
    printf("   CLI - NETCONF Console (C / Direct Mode)\n");
    printf("============================================%s\n\n", COLOR_RESET);
    printf("Connecting to %s%s:%d%s (mode=%s) ...\n",
           COLOR_BOLD, host, port, COLOR_RESET, mode);

    /* Kết nối NETCONF */
    netconf_session_t *nc = NULL;
    if (strcasecmp(mode, "ssh") == 0) {
#ifdef WITH_SSH
        nc = nc_dial_ssh(host, port,
                         env_or("NETCONF_USER", "admin"),
                         env_or("NETCONF_PASS", "admin"));
#else
        fprintf(stderr, "%sCompiled without SSH support. "
                "Use make WITH_SSH=1 direct%s\n", COLOR_RED, COLOR_RESET);
        return 1;
#endif
    } else {
        nc = nc_dial_tcp(host, port);
    }

    if (!nc) {
        fprintf(stderr, "%sConnection failed.%s\n", COLOR_RED, COLOR_RESET);
        return 1;
    }

    printf("%sConnected.%s NETCONF session ID: %s\n\n",
           COLOR_GREEN, COLOR_RESET, nc->session_id);

    /* Khởi tạo session */
    memset(&g_session, 0, sizeof(g_session));
    strncpy(g_session.username, "direct", sizeof(g_session.username) - 1);
    g_session.nc = nc;

    /* Cài NE info (dùng static NE cho direct mode) */
    ne_info_t *ne = &g_session.nes[0];
    strncpy(ne->name, ne_name, sizeof(ne->name) - 1);
    strncpy(ne->ip,   host,    sizeof(ne->ip)   - 1);
    ne->port = port;
    g_session.ne_count  = 1;
    g_session.current_ne = ne;

    update_prompt(&g_session);

    /* Readline */
    using_history();
    stifle_history(HISTORY_SIZE);
    completer_init(&g_session);

    /* Load schema */
    printf("Loading schema...\n");
    schema_load(&g_session);
    printf("Type 'help' for available commands. Ctrl+D or 'exit' to quit.\n\n");

    /* Command loop */
    command_loop(&g_session);

    /* Cleanup */
    printf("Goodbye.\n");
    nc_close(nc);
    schema_free(g_session.schema);
    curl_global_cleanup();
    xmlCleanupParser();
    return 0;
}
