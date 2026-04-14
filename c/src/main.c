/*
 * main.c — Entry point, auth flow, NE selection loop, command loop
 *
 * Chạy như local terminal application (readline trực tiếp trên stdin/stdout).
 * Các env vars cấu hình:
 *   MGT_URL          http://127.0.0.1:3000
 *   NETCONF_HOST     127.0.0.1
 *   NETCONF_PORT     2023  (TCP) hoặc 8830 (SSH)
 *   NETCONF_MODE     tcp | ssh   (default: tcp)
 *   NETCONF_USER     admin
 *   NETCONF_PASS     admin
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

/* -------------------------------------------------------------------------
 * Global session (một process = một session trong direct mode)
 * ---------------------------------------------------------------------- */
static cli_session_t g_session;

/* -------------------------------------------------------------------------
 * Signal handler — Ctrl+C không thoát, chỉ cancel dòng hiện tại
 * (readline tự xử lý SIGINT khi rl_catch_signals = 1)
 * ---------------------------------------------------------------------- */
static void sigint_handler(int sig) {
    (void)sig;
    printf("\n");
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

/* -------------------------------------------------------------------------
 * update_prompt
 * ---------------------------------------------------------------------- */
static void update_prompt(cli_session_t *s) {
    if (s->current_ne) {
        snprintf(s->prompt, sizeof(s->prompt),
                 "%s%s%s[%s%s%s]> ",
                 COLOR_CYAN, s->username, COLOR_RESET,
                 COLOR_YELLOW, s->current_ne->name, COLOR_RESET);
    } else {
        snprintf(s->prompt, sizeof(s->prompt),
                 "%s%s%s> ", COLOR_CYAN, s->username, COLOR_RESET);
    }
}

/* -------------------------------------------------------------------------
 * print_welcome
 * ---------------------------------------------------------------------- */
static void print_welcome(void) {
    printf("%s", COLOR_GREEN);
    printf("============================================\n");
    printf("        CLI - NETCONF Console (C/MAAPI)\n");
    printf("============================================%s\n\n", COLOR_RESET);
}

/* -------------------------------------------------------------------------
 * display_ne_table
 * ---------------------------------------------------------------------- */
static void display_ne_table(cli_session_t *s) {
    printf("%s  #  NE              Site    IP              Port  Description%s\n",
           COLOR_BOLD, COLOR_RESET);
    for (int i = 0; i < s->ne_count; i++) {
        ne_info_t *ne = &s->nes[i];
        printf("  %d  %-15s %-7s %-15s %-5d %s\n",
               i + 1, ne->name, ne->site, ne->ip, ne->port, ne->description);
    }
}

/* -------------------------------------------------------------------------
 * resolve_ne — trả về index (0-based) hoặc -1
 * ---------------------------------------------------------------------- */
static int resolve_ne(cli_session_t *s, const char *input) {
    /* Thử số */
    char *endp;
    long idx = strtol(input, &endp, 10);
    if (*endp == '\0' && idx >= 1 && idx <= s->ne_count)
        return (int)idx - 1;
    /* Thử tên (case-insensitive) */
    for (int i = 0; i < s->ne_count; i++) {
        if (strcasecmp(s->nes[i].name, input) == 0) return i;
    }
    return -1;
}

/* -------------------------------------------------------------------------
 * do_connect — kết nối tới NE và load schema
 * ---------------------------------------------------------------------- */
static bool do_connect(cli_session_t *s, int ne_idx) {
    ne_info_t *ne = &s->nes[ne_idx];

    /* Đọc env config */
    const char *mode = getenv("NETCONF_MODE");
    const char *host = getenv("NETCONF_HOST");
    const char *port_str = getenv("NETCONF_PORT");
    const char *user = getenv("NETCONF_USER");
    const char *pass = getenv("NETCONF_PASS");

    if (!mode) mode = "tcp";
    if (!host) host = ne->ip;
    int port = port_str ? atoi(port_str) : ne->port;
    if (!user) user = "admin";
    if (!pass) pass = "admin";

    printf("Connecting to %s (%s:%d) via %s...\n",
           ne->name, host, port, mode);

    netconf_session_t *nc = NULL;
    if (strcasecmp(mode, "ssh") == 0) {
        nc = nc_dial_ssh(host, port, user, pass);
    } else {
        nc = nc_dial_tcp(host, port);
    }

    if (!nc) {
        fprintf(stderr, "%sConnection failed.%s\n", COLOR_RED, COLOR_RESET);
        return false;
    }

    s->nc         = nc;
    s->current_ne = ne;
    update_prompt(s);
    completer_update_session(s);

    printf("%sConnected.%s NETCONF session ID: %s\n",
           COLOR_GREEN, COLOR_RESET, nc->session_id);

    /* Load schema (MAAPI → get-schema → XML fallback) */
    printf("Loading schema...\n");
    schema_load(s);

    /* Load backups từ mgt-service */
    auth_list_backups(s);

    return true;
}

/* -------------------------------------------------------------------------
 * do_disconnect
 * ---------------------------------------------------------------------- */
static void do_disconnect(cli_session_t *s) {
    if (!s->nc) return;
    const char *name = s->current_ne ? s->current_ne->name : "?";
    nc_close(s->nc);
    s->nc = NULL;
    s->current_ne = NULL;

    /* Free backups */
    if (s->backups) {
        for (int i = 0; i < s->backup_count; i++) free(s->backups[i].xml);
        free(s->backups);
        s->backups = NULL;
        s->backup_count = 0;
        s->backup_seq = 0;
    }

    /* Free schema */
    schema_free(s->schema);
    s->schema = NULL;

    update_prompt(s);
    completer_update_session(s);
    printf("Disconnected from %s.\n", name);
}

/* -------------------------------------------------------------------------
 * ne_selection_loop — hiện bảng NE và chờ user chọn
 * Trả về true nếu đã kết nối thành công, false nếu user gõ exit
 * ---------------------------------------------------------------------- */
static bool ne_selection_loop(cli_session_t *s) {
    display_ne_table(s);
    printf("\n");

    char prompt_buf[128];
    snprintf(prompt_buf, sizeof(prompt_buf),
             "Select NE [1-%d or name] (exit to quit): ", s->ne_count);

    while (1) {
        char *line = readline(prompt_buf);
        if (!line) return false; /* EOF / Ctrl+D */

        char *trimmed = str_trim(line);
        if (!*trimmed) { free(line); continue; }

        if (strcasecmp(trimmed, "exit") == 0) { free(line); return false; }

        int idx = resolve_ne(s, trimmed);
        if (idx < 0) {
            printf("%sNE '%s' not found.%s\n",
                   COLOR_RED, trimmed, COLOR_RESET);
            free(line);
            continue;
        }

        bool ok = do_connect(s, idx);
        free(line);
        return ok;
    }
}

/* -------------------------------------------------------------------------
 * command_loop — chạy vòng lặp lệnh sau khi đã kết nối
 * Trả về true → thoát hẳn SSH, false → về màn hình chọn NE
 * ---------------------------------------------------------------------- */
static bool command_loop(cli_session_t *s) {
    while (1) {
        char *line = readline(s->prompt);
        if (!line) {
            /* Ctrl+D → thoát */
            do_disconnect(s);
            return true;
        }

        char *trimmed = str_trim(line);
        if (!*trimmed) { free(line); continue; }

        add_history(trimmed);

        /* Xử lý connect / disconnect / exit riêng */
        int argc = 0;
        char **argv = str_split(trimmed, &argc);

        if (argc > 0) {
            if (strcasecmp(argv[0], "connect") == 0) {
                if (s->nc) {
                    printf("Already connected to %s. Use 'disconnect' first.\n",
                           s->current_ne->name);
                } else if (argc < 2) {
                    printf("Usage: connect <name|number>\n");
                } else {
                    int idx = resolve_ne(s, argv[1]);
                    if (idx < 0) {
                        printf("%sNE '%s' not found.%s\n",
                               COLOR_RED, argv[1], COLOR_RESET);
                    } else {
                        do_connect(s, idx);
                    }
                }
            } else if (strcasecmp(argv[0], "disconnect") == 0) {
                do_disconnect(s);
                free_tokens(argv, argc);
                free(line);
                return false; /* về NE selection */
            } else if (strcasecmp(argv[0], "exit") == 0 ||
                       strcasecmp(argv[0], "quit") == 0) {
                if (s->nc) {
                    do_disconnect(s);
                    free_tokens(argv, argc);
                    free(line);
                    return false; /* về NE selection */
                } else {
                    free_tokens(argv, argc);
                    free(line);
                    return true; /* thoát hẳn */
                }
            } else {
                cmd_dispatch(s, trimmed);
            }
        }

        free_tokens(argv, argc);
        free(line);
    }
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */
int main(void) {
    /* Khởi tạo thư viện */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    xmlInitParser();
    signal(SIGINT, sigint_handler);
    rl_catch_signals = 0; /* ta tự xử lý SIGINT */

    print_welcome();

    /* Khởi tạo session */
    memset(&g_session, 0, sizeof(g_session));
    const char *mgt = getenv("MGT_URL");
    strncpy(g_session.mgt_url, mgt ? mgt : "http://127.0.0.1:3000",
            sizeof(g_session.mgt_url) - 1);

    /* Readline history */
    using_history();
    stifle_history(HISTORY_SIZE);

    /* Init completer */
    completer_init(&g_session);

    /* ---------- Auth ---------- */
    printf("Login to mgt-service (%s)\n", g_session.mgt_url);

    char *username = readline("Username: ");
    if (!username) goto cleanup;
    str_trim(username);

    /* Password (không echo) */
    rl_bind_key('\t', rl_insert); /* tạm tắt tab completion khi nhập password */
    char *password = readline("Password: ");
    rl_bind_key('\t', rl_complete);
    if (!password) { free(username); goto cleanup; }
    str_trim(password);

    if (!auth_login(&g_session, username, password)) {
        fprintf(stderr, "%sAuthentication failed.%s\n", COLOR_RED, COLOR_RESET);
        free(username); free(password);
        goto cleanup;
    }
    free(password);

    strncpy(g_session.username, username, sizeof(g_session.username) - 1);
    free(username);
    printf("%sAuthenticated.%s\n\n", COLOR_GREEN, COLOR_RESET);

    /* Load NE list */
    if (!auth_list_ne(&g_session)) {
        fprintf(stderr, "Failed to load NE list.\n");
        goto cleanup;
    }
    if (g_session.ne_count == 0) {
        fprintf(stderr, "No network elements available.\n");
        goto cleanup;
    }

    /* ---------- Main loop ---------- */
    while (1) {
        bool quit = !ne_selection_loop(&g_session);
        if (quit) break;

        bool exit_all = command_loop(&g_session);
        if (exit_all) break;
        /* Nếu false → quay lại NE selection */
    }

    printf("Goodbye.\n");

cleanup:
    if (g_session.nc) nc_close(g_session.nc);
    schema_free(g_session.schema);
    if (g_session.backups) {
        for (int i = 0; i < g_session.backup_count; i++)
            free(g_session.backups[i].xml);
        free(g_session.backups);
    }
    curl_global_cleanup();
    xmlCleanupParser();
    return 0;
}
