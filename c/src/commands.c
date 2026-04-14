/*
 * commands.c — tất cả command handlers cho CLI
 *
 * Commands: show, set, unset, commit, validate, discard,
 *           lock, unlock, dump, rpc, restore, connect,
 *           disconnect, help, exit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "cli.h"

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static void print_ok(void) {
    printf("%sOK%s\n", COLOR_GREEN, COLOR_RESET);
}

static void print_err(const char *msg) {
    fprintf(stderr, "%sError: %s%s\n", COLOR_RED, msg, COLOR_RESET);
}

static void print_elapsed(long ms) {
    printf("%s(%ldms)%s\n", COLOR_DIM, ms, COLOR_RESET);
}

static long elapsed_ms(struct timeval *start) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec  - start->tv_sec)  * 1000 +
           (now.tv_usec - start->tv_usec) / 1000;
}

/* Pager: in text có phân trang 20 dòng */
static void paged_print(const char *text) {
    if (!text) return;
    const char *p = text;
    int lines = 0;

    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
        fwrite(p, 1, len, stdout);
        p += len;
        lines++;

        if (lines % PAGE_SIZE == 0 && *p) {
            /* Count remaining lines */
            int remaining = 0;
            const char *q = p;
            while (*q) { if (*q == '\n') remaining++; q++; }
            printf("%s<MORE>%s — %d lines left"
                   "  [Enter] next  [a/G] all  [q] quit ",
                   COLOR_YELLOW, COLOR_RESET, remaining);
            fflush(stdout);

            char ch = (char)getchar();
            /* Xoá dòng <MORE> */
            printf("\r\033[2K");

            if (ch == 'q' || ch == 'Q' || ch == 27) return;
            if (ch == 'a' || ch == 'A' || ch == 'G' || ch == 'g') {
                /* Hiện toàn bộ còn lại */
                printf("%s", p);
                return;
            }
            /* Enter / Space → next page */
        }
    }
}

/* -------------------------------------------------------------------------
 * Require connection
 * ---------------------------------------------------------------------- */
static bool require_connection(cli_session_t *s) {
    if (!s->nc) {
        print_err("Not connected. Use 'connect <name>'");
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * cmd_show
 * ---------------------------------------------------------------------- */
static void cmd_show_ne(cli_session_t *s) {
    printf("%s  #  NE              Site    IP              Port  Description%s\n",
           COLOR_BOLD, COLOR_RESET);
    for (int i = 0; i < s->ne_count; i++) {
        ne_info_t *ne = &s->nes[i];
        printf("  %d  %-15s %-7s %-15s %-5d %s\n",
               i + 1, ne->name, ne->site, ne->ip, ne->port, ne->description);
    }
}

static void cmd_show_config(cli_session_t *s, const char *datastore,
                             const char **path, int path_len) {
    if (!require_connection(s)) return;

    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_get_config(s->nc, datastore, "");
    long ms = elapsed_ms(&t0);

    if (!reply) { print_err("get-config failed"); return; }

    char *text = fmt_xml_to_text(reply, path, path_len);
    free(reply);

    paged_print(text);
    free(text);
    print_elapsed(ms);
    auth_save_history(s, "show", ms);
}

static void cmd_show_backups(cli_session_t *s) {
    if (s->backup_count == 0) {
        printf("No backups. Snapshots are created after each commit.\n");
        return;
    }
    printf("%s  ID  Timestamp              Size     Source%s\n",
           COLOR_BOLD, COLOR_RESET);
    for (int i = 0; i < s->backup_count; i++) {
        backup_t *b = &s->backups[i];
        printf("  %s%2d%s  %-22s  %-8s %s\n",
               COLOR_YELLOW, b->id, COLOR_RESET,
               b->timestamp,
               b->xml ? "local" : "-",
               b->remote_id ? "remote" : "local");
    }
    printf("\nUse '%srestore <id>%s' to roll back.\n", COLOR_CYAN, COLOR_RESET);
}

static void cmd_show(cli_session_t *s, char **args, int argc) {
    if (argc == 0) {
        printf("Usage: show ne | running-config | candidate-config | backups\n");
        return;
    }
    if (strcasecmp(args[0], "ne") == 0) {
        cmd_show_ne(s);
    } else if (strcasecmp(args[0], "running-config") == 0) {
        cmd_show_config(s, "running",
                        (const char **)args + 1, argc - 1);
    } else if (strcasecmp(args[0], "candidate-config") == 0) {
        cmd_show_config(s, "candidate",
                        (const char **)args + 1, argc - 1);
    } else if (strcasecmp(args[0], "backups") == 0) {
        cmd_show_backups(s);
    } else {
        printf("Unknown: show %s\n", args[0]);
    }
}

/* -------------------------------------------------------------------------
 * build_set_xml — tạo XML từ path + value
 * ---------------------------------------------------------------------- */
static char *build_set_xml(cli_session_t *s, const char **path, int depth,
                            const char *value) {
    if (!s->schema || depth == 0) return NULL;

    /* Tìm namespace của top-level container */
    char ns[MAX_NS_LEN] = {0};
    for (schema_node_t *c = s->schema->children; c; c = c->next) {
        if (strcasecmp(c->name, path[0]) == 0) {
            strncpy(ns, c->ns, MAX_NS_LEN - 1);
            break;
        }
    }

    size_t buf_cap = 4096;
    char  *buf     = malloc(buf_cap);
    if (!buf) return NULL;
    int pos = 0;

    /* Opening tags */
    if (ns[0]) {
        pos += snprintf(buf + pos, buf_cap - pos,
                        "<%s xmlns=\"%s\">", path[0], ns);
    } else {
        pos += snprintf(buf + pos, buf_cap - pos, "<%s>", path[0]);
    }
    for (int i = 1; i < depth - 1; i++) {
        pos += snprintf(buf + pos, buf_cap - pos, "<%s>", path[i]);
    }

    /* Leaf */
    const char *leaf = path[depth - 1];
    pos += snprintf(buf + pos, buf_cap - pos,
                    "<%s>%s</%s>", leaf, value, leaf);

    /* Closing tags */
    for (int i = depth - 2; i >= 1; i--) {
        pos += snprintf(buf + pos, buf_cap - pos, "</%s>", path[i]);
    }
    pos += snprintf(buf + pos, buf_cap - pos, "</%s>", path[0]);

    return buf;
}

/* build_unset_xml */
static char *build_unset_xml(cli_session_t *s, const char **path, int depth) {
    if (!s->schema || depth == 0) return NULL;
    char ns[MAX_NS_LEN] = {0};
    for (schema_node_t *c = s->schema->children; c; c = c->next) {
        if (strcasecmp(c->name, path[0]) == 0) {
            strncpy(ns, c->ns, MAX_NS_LEN - 1);
            break;
        }
    }

    char *buf = malloc(4096);
    if (!buf) return NULL;
    int pos = 0;
    const char *nc_ns = "urn:ietf:params:xml:ns:netconf:base:1.0";

    if (ns[0]) {
        pos += snprintf(buf + pos, 4096 - pos,
                        "<%s xmlns=\"%s\" xmlns:nc=\"%s\">",
                        path[0], ns, nc_ns);
    } else {
        pos += snprintf(buf + pos, 4096 - pos,
                        "<%s xmlns:nc=\"%s\">", path[0], nc_ns);
    }
    for (int i = 1; i < depth - 1; i++)
        pos += snprintf(buf + pos, 4096 - pos, "<%s>", path[i]);

    pos += snprintf(buf + pos, 4096 - pos,
                    "<%s nc:operation=\"delete\"/>", path[depth - 1]);

    for (int i = depth - 2; i >= 1; i--)
        pos += snprintf(buf + pos, 4096 - pos, "</%s>", path[i]);
    pos += snprintf(buf + pos, 4096 - pos, "</%s>", path[0]);
    return buf;
}

/* -------------------------------------------------------------------------
 * cmd_set
 * ---------------------------------------------------------------------- */
static void cmd_set(cli_session_t *s, char **args, int argc) {
    if (!require_connection(s)) return;

    char *xml = NULL;

    if (argc >= 2) {
        /* set path... value */
        const char **path = (const char **)args;
        int depth = argc - 1;
        xml = build_set_xml(s, path, depth, args[argc - 1]);
        if (!xml) { print_err("Invalid path"); return; }
    } else if (argc == 0) {
        /* Paste mode */
        printf("Enter config (XML or text format, end with '%s.%s' on a new line):\n",
               COLOR_YELLOW, COLOR_RESET);
        size_t cap = 65536, len = 0;
        char *input = malloc(cap);
        if (!input) return;

        char line[MAX_LINE_LEN];
        while (fgets(line, sizeof(line), stdin)) {
            if (strcmp(str_trim(line), ".") == 0) break;
            size_t ll = strlen(line);
            while (len + ll + 1 >= cap) {
                cap *= 2;
                input = realloc(input, cap);
                if (!input) return;
            }
            memcpy(input + len, line, ll);
            len += ll;
            input[len] = '\0';
        }
        if (len == 0) { printf("Cancelled.\n"); free(input); return; }

        /* Detect XML vs text */
        char *trimmed = str_trim(input);
        if (trimmed[0] == '<') {
            xml = xstrdup(trimmed);
        } else {
            xml = fmt_text_to_xml(trimmed, s->schema);
        }
        free(input);
    } else {
        printf("Usage: set <path...> <value>\n");
        printf("       set             (paste mode)\n");
        return;
    }

    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_edit_config(s->nc, "candidate", xml);
    long ms = elapsed_ms(&t0);
    free(xml);

    if (!reply) { print_err("edit-config failed"); return; }
    if (fmt_is_rpc_ok(reply))      print_ok();
    else if (fmt_is_rpc_error(reply)) {
        char *msg = fmt_extract_error_msg(reply);
        print_err(msg);
        free(msg);
    } else printf("%s\n", reply);
    free(reply);
    print_elapsed(ms);
    auth_save_history(s, "set", ms);
}

/* -------------------------------------------------------------------------
 * cmd_unset
 * ---------------------------------------------------------------------- */
static void cmd_unset(cli_session_t *s, char **args, int argc) {
    if (!require_connection(s)) return;
    if (argc == 0) { printf("Usage: unset <path...>\n"); return; }

    char *xml = build_unset_xml(s, (const char **)args, argc);
    if (!xml) { print_err("Invalid path"); return; }

    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_edit_config(s->nc, "candidate", xml);
    long ms = elapsed_ms(&t0);
    free(xml);

    if (!reply) { print_err("edit-config failed"); return; }
    if (fmt_is_rpc_ok(reply))       print_ok();
    else if (fmt_is_rpc_error(reply)) {
        char *msg = fmt_extract_error_msg(reply);
        print_err(msg);
        free(msg);
    } else printf("%s\n", reply);
    free(reply);
    print_elapsed(ms);
    auth_save_history(s, "unset", ms);
}

/* -------------------------------------------------------------------------
 * cmd_commit + capture_backup
 * ---------------------------------------------------------------------- */
static void capture_backup(cli_session_t *s) {
    char *reply = nc_get_config(s->nc, "running", "");
    if (!reply) return;

    char *raw = fmt_extract_raw_data(reply);
    free(reply);
    if (!raw) return;

    s->backup_seq++;
    /* Grow backup array */
    backup_t *nb = realloc(s->backups,
                           (s->backup_count + 1) * sizeof(backup_t));
    if (!nb) { free(raw); return; }
    s->backups = nb;

    backup_t *b = &s->backups[s->backup_count];
    b->id        = s->backup_seq;
    b->remote_id = 0;
    b->xml       = raw;
    time_t now   = time(NULL);
    struct tm *tm = localtime(&now);
    strftime(b->timestamp, sizeof(b->timestamp),
             "%Y-%m-%d %H:%M:%S", tm);

    /* Lưu lên mgt-service */
    int remote_id = 0;
    if (auth_save_backup(s, raw, &remote_id))
        b->remote_id = remote_id;

    s->backup_count++;
}

static void cmd_commit(cli_session_t *s) {
    if (!require_connection(s)) return;

    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_commit(s->nc);
    long ms = elapsed_ms(&t0);

    if (!reply) { print_err("commit failed"); return; }
    if (fmt_is_rpc_ok(reply)) {
        printf("%sCommit successful.%s\n", COLOR_GREEN, COLOR_RESET);
        capture_backup(s);
    } else if (fmt_is_rpc_error(reply)) {
        char *msg = fmt_extract_error_msg(reply);
        printf("%sCommit failed: %s%s\n", COLOR_RED, msg, COLOR_RESET);
        free(msg);
    } else printf("%s\n", reply);
    free(reply);
    print_elapsed(ms);
    auth_save_history(s, "commit", ms);
}

/* -------------------------------------------------------------------------
 * cmd_validate
 * ---------------------------------------------------------------------- */
static void cmd_validate(cli_session_t *s) {
    if (!require_connection(s)) return;
    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_validate(s->nc, "candidate");
    long ms = elapsed_ms(&t0);
    if (!reply) { print_err("validate failed"); return; }
    if (fmt_is_rpc_ok(reply))
        printf("%sValidation OK.%s\n", COLOR_GREEN, COLOR_RESET);
    else if (fmt_is_rpc_error(reply)) {
        char *msg = fmt_extract_error_msg(reply);
        printf("%sValidation failed: %s%s\n", COLOR_RED, msg, COLOR_RESET);
        free(msg);
    }
    free(reply);
    print_elapsed(ms);
    auth_save_history(s, "validate", ms);
}

/* -------------------------------------------------------------------------
 * cmd_discard
 * ---------------------------------------------------------------------- */
static void cmd_discard(cli_session_t *s) {
    if (!require_connection(s)) return;
    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_discard(s->nc);
    long ms = elapsed_ms(&t0);
    if (!reply) { print_err("discard failed"); return; }
    if (fmt_is_rpc_ok(reply))
        printf("%sChanges discarded.%s\n", COLOR_GREEN, COLOR_RESET);
    else printf("%s\n", reply);
    free(reply);
    auth_save_history(s, "discard", ms);
}

/* -------------------------------------------------------------------------
 * cmd_lock / cmd_unlock
 * ---------------------------------------------------------------------- */
static void cmd_lock(cli_session_t *s, char **args, int argc) {
    if (!require_connection(s)) return;
    const char *ds = (argc > 0) ? args[0] : "candidate";
    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_lock(s->nc, ds);
    long ms = elapsed_ms(&t0);
    if (!reply) { print_err("lock failed"); return; }
    if (fmt_is_rpc_ok(reply))
        printf("%sLocked %s.%s\n", COLOR_GREEN, ds, COLOR_RESET);
    else if (fmt_is_rpc_error(reply)) {
        char *msg = fmt_extract_error_msg(reply);
        printf("%sLock failed: %s%s\n", COLOR_RED, msg, COLOR_RESET);
        free(msg);
    }
    free(reply);
    auth_save_history(s, "lock", ms);
}

static void cmd_unlock(cli_session_t *s, char **args, int argc) {
    if (!require_connection(s)) return;
    const char *ds = (argc > 0) ? args[0] : "candidate";
    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_unlock(s->nc, ds);
    long ms = elapsed_ms(&t0);
    if (!reply) { print_err("unlock failed"); return; }
    if (fmt_is_rpc_ok(reply))
        printf("%sUnlocked %s.%s\n", COLOR_GREEN, ds, COLOR_RESET);
    free(reply);
    auth_save_history(s, "unlock", ms);
}

/* -------------------------------------------------------------------------
 * cmd_dump
 * ---------------------------------------------------------------------- */
static void cmd_dump(cli_session_t *s, char **args, int argc) {
    if (!require_connection(s)) return;
    if (argc == 0) {
        printf("Usage: dump text|xml [running|candidate] [filename]\n");
        return;
    }

    /* dump <format> [running|candidate] [file] */
    const char *format    = args[0];
    const char *datastore = "running";
    const char *filename  = NULL;

    if (strcasecmp(format, "text") != 0 && strcasecmp(format, "xml") != 0) {
        print_err("Format must be 'text' or 'xml'");
        return;
    }

    int next = 1;
    if (argc > next &&
        (strcasecmp(args[next], "running")   == 0 ||
         strcasecmp(args[next], "candidate") == 0)) {
        datastore = args[next++];
    }
    if (argc > next) {
        filename = args[next];
    }

    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_get_config(s->nc, datastore, "");
    long ms = elapsed_ms(&t0);
    if (!reply) { print_err("get-config failed"); return; }

    char *content = NULL;
    if (strcasecmp(format, "text") == 0) {
        content = fmt_xml_to_text(reply, NULL, 0);
    } else {
        content = fmt_extract_data_xml(reply);
    }
    free(reply);
    if (!content) { print_err("empty config"); return; }

    if (filename) {
        FILE *f = fopen(filename, "w");
        if (!f) { print_err("Cannot open file"); free(content); return; }
        fwrite(content, 1, strlen(content), f);
        fclose(f);
        printf("%sSaved to %s (%zu bytes)%s\n",
               COLOR_GREEN, filename, strlen(content), COLOR_RESET);
    } else {
        paged_print(content);
    }
    free(content);
    print_elapsed(ms);

    char hist_cmd[32];
    snprintf(hist_cmd, sizeof(hist_cmd), "dump-%s-%s", format, datastore);
    auth_save_history(s, hist_cmd, ms);
}

/* -------------------------------------------------------------------------
 * cmd_rpc — paste raw RPC XML
 * ---------------------------------------------------------------------- */
static void cmd_rpc(cli_session_t *s) {
    if (!require_connection(s)) return;
    printf("Enter RPC body XML (end with '%s.%s' on a new line):\n",
           COLOR_YELLOW, COLOR_RESET);

    size_t cap = 65536, len = 0;
    char *body = malloc(cap);
    if (!body) return;
    char line[MAX_LINE_LEN];
    while (fgets(line, sizeof(line), stdin)) {
        if (strcmp(str_trim(line), ".") == 0) break;
        size_t ll = strlen(line);
        while (len + ll + 1 >= cap) { cap *= 2; body = realloc(body, cap); }
        memcpy(body + len, line, ll);
        len += ll;
        body[len] = '\0';
    }

    struct timeval t0; gettimeofday(&t0, NULL);
    char *reply = nc_send_rpc(s->nc, body);
    long ms = elapsed_ms(&t0);
    free(body);

    if (reply) printf("%s\n", reply);
    free(reply);
    print_elapsed(ms);
}

/* -------------------------------------------------------------------------
 * cmd_restore
 * ---------------------------------------------------------------------- */
static void cmd_restore(cli_session_t *s, char **args, int argc) {
    if (!require_connection(s)) return;
    if (argc == 0) {
        printf("Usage: restore <id>\n");
        return;
    }

    int id = atoi(args[0]);
    backup_t *target = NULL;
    for (int i = 0; i < s->backup_count; i++) {
        if (s->backups[i].id == id) { target = &s->backups[i]; break; }
    }
    if (!target) {
        printf("%sBackup #%d not found.%s\n", COLOR_RED, id, COLOR_RESET);
        return;
    }

    /* Lazy fetch từ mgt-service */
    if (!target->xml && target->remote_id != 0) {
        printf("Fetching backup from server...\n");
        if (!auth_get_backup(s, target->remote_id, &target->xml)) {
            print_err("Failed to fetch backup");
            return;
        }
    }
    if (!target->xml) { print_err("Backup data is empty"); return; }

    printf("Restore to snapshot #%d (%s)? [y/N] ", id, target->timestamp);
    fflush(stdout);
    char confirm[16];
    if (!fgets(confirm, sizeof(confirm), stdin)) return;
    str_trim(confirm);
    if (strcasecmp(confirm, "y") != 0) { printf("Cancelled.\n"); return; }

    printf("Restoring...\n");
    char *reply = nc_copy_config(s->nc, "candidate", target->xml);
    if (!reply || fmt_is_rpc_error(reply)) {
        char *msg = reply ? fmt_extract_error_msg(reply) : xstrdup("copy-config failed");
        printf("%sRestore failed: %s%s\n", COLOR_RED, msg, COLOR_RESET);
        free(msg); free(reply);
        return;
    }
    free(reply);

    reply = nc_commit(s->nc);
    if (reply && fmt_is_rpc_ok(reply)) {
        printf("%sRestored to snapshot #%d.%s\n", COLOR_GREEN, id, COLOR_RESET);
        capture_backup(s);
    } else if (reply && fmt_is_rpc_error(reply)) {
        char *msg = fmt_extract_error_msg(reply);
        printf("%sCommit failed: %s%s\n", COLOR_RED, msg, COLOR_RESET);
        free(msg);
    }
    free(reply);
}

/* -------------------------------------------------------------------------
 * cmd_help
 * ---------------------------------------------------------------------- */
static void cmd_help(void) {
    printf(
        "%sGeneral commands:%s\n"
        "  show ne                           List network elements\n"
        "  connect <name>                    Connect to NE\n"
        "  disconnect                        Close NETCONF session\n"
        "  help                              Show this help\n"
        "  exit                              Exit\n"
        "\n"
        "%sConfiguration (requires connection):%s\n"
        "  show running-config [path...]     Show running config\n"
        "  show candidate-config [path...]   Show candidate config\n"
        "  show backups                      List config snapshots\n"
        "  set <path...> <value>             Set config value\n"
        "  set                               Paste config (XML or text)\n"
        "  unset <path...>                   Delete config node\n"
        "  commit                            Commit candidate\n"
        "  validate                          Validate candidate\n"
        "  discard                           Discard changes\n"
        "  lock [datastore]                  Lock datastore\n"
        "  unlock [datastore]                Unlock datastore\n"
        "  dump text [file]                  Export config as text\n"
        "  dump xml  [file]                  Export config as XML\n"
        "  rpc                               Send raw NETCONF RPC\n"
        "  restore <id>                      Restore a config snapshot\n"
        "\n"
        "%sTab completion:%s press Tab to complete commands and config paths.\n",
        COLOR_BOLD, COLOR_RESET,
        COLOR_BOLD, COLOR_RESET,
        COLOR_DIM,  COLOR_RESET
    );
}

/* -------------------------------------------------------------------------
 * cmd_dispatch — main entry từ main loop
 * ---------------------------------------------------------------------- */
void cmd_dispatch(cli_session_t *s, const char *line) {
    if (!line || !*line) return;

    int    argc = 0;
    char **argv = str_split(line, &argc);
    if (argc == 0) return;

    const char *cmd = argv[0];

    if (strcasecmp(cmd, "show") == 0)
        cmd_show(s, argv + 1, argc - 1);
    else if (strcasecmp(cmd, "set") == 0)
        cmd_set(s, argv + 1, argc - 1);
    else if (strcasecmp(cmd, "unset") == 0)
        cmd_unset(s, argv + 1, argc - 1);
    else if (strcasecmp(cmd, "commit") == 0)
        cmd_commit(s);
    else if (strcasecmp(cmd, "validate") == 0)
        cmd_validate(s);
    else if (strcasecmp(cmd, "discard") == 0)
        cmd_discard(s);
    else if (strcasecmp(cmd, "lock") == 0)
        cmd_lock(s, argv + 1, argc - 1);
    else if (strcasecmp(cmd, "unlock") == 0)
        cmd_unlock(s, argv + 1, argc - 1);
    else if (strcasecmp(cmd, "dump") == 0)
        cmd_dump(s, argv + 1, argc - 1);
    else if (strcasecmp(cmd, "rpc") == 0)
        cmd_rpc(s);
    else if (strcasecmp(cmd, "restore") == 0)
        cmd_restore(s, argv + 1, argc - 1);
    else if (strcasecmp(cmd, "help") == 0)
        cmd_help();
    else
        printf("%sUnknown command: %s%s  (type 'help')\n",
               COLOR_RED, cmd, COLOR_RESET);

    free_tokens(argv, argc);
}
