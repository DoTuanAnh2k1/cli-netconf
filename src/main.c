/*
 * main.c — CLI NETCONF via ConfD MAAPI
 *
 * Connects directly to ConfD IPC port (default :4565).
 * All get-config / set / commit operations go through MAAPI.
 *
 * Build:
 *   make CONFD_DIR=/path/to/confd
 *
 * Run:
 *   CONFD_IPC_ADDR=127.0.0.1 CONFD_IPC_PORT=4565 ./cli-netconf
 *
 * Env vars:
 *   CONFD_IPC_ADDR   ConfD host    (default: 127.0.0.1)
 *   CONFD_IPC_PORT   ConfD port    (default: 4565)
 *   MAAPI_USER       user label    (default: admin)
 *   NE_NAME          prompt label  (default: confd)
 *
 * Commands:
 *   show running-config [path...]
 *   show candidate-config [path...]
 *   set <path...> <value>     — set leaf (space-separated or /keypath)
 *   set                       — paste XML config block (empty line to finish)
 *   unset <path...>           — delete node
 *   commit
 *   validate
 *   discard
 *   lock [running|candidate]
 *   unlock [running|candidate]
 *   dump xml [file]
 *   dump text [file]
 *   help
 *   exit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include "confd_compat.h"
#include <readline/readline.h>
#include <readline/history.h>
#include "cli.h"
#include "maapi-direct.h"

/* ─── Globals ────────────────────────────────────────────── */
static maapi_session_t *g_maapi   = NULL;
static schema_node_t   *g_schema  = NULL;
static char             g_prompt[256];
static char             g_ne_name[128] = "confd";

/* ─── Helpers ─────────────────────────────────────────────── */

static const char *env_or(const char *k, const char *d) {
    const char *v = getenv(k); return (v && *v) ? v : d;
}
static int env_int_or(const char *k, int d) {
    const char *v = getenv(k); return (v && *v) ? atoi(v) : d;
}

static long elapsed_ms(struct timeval *t0) {
    struct timeval now; gettimeofday(&now, NULL);
    return (now.tv_sec - t0->tv_sec) * 1000 +
           (now.tv_usec - t0->tv_usec) / 1000;
}

static void sigint_handler(int sig) {
    (void)sig;
    printf("\n");
    rl_on_new_line();
    rl_replace_line("", 0);
    rl_redisplay();
}

static void update_prompt(void) {
    snprintf(g_prompt, sizeof(g_prompt),
             "%smaapi%s[%s%s%s]> ",
             COLOR_CYAN, COLOR_RESET,
             COLOR_YELLOW, g_ne_name, COLOR_RESET);
}

/* Pager đơn giản */
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
            int rem = 0; const char *q = p;
            while (*q) { if (*q == '\n') rem++; q++; }
            printf("%s<MORE>%s [Enter] next [a] all [q] quit — %d lines left ",
                   COLOR_YELLOW, COLOR_RESET, rem);
            fflush(stdout);
            char ch = (char)getchar();
            printf("\r\033[2K");
            if (ch == 'q' || ch == 'Q' || ch == 27) return;
            if (ch == 'a' || ch == 'A' || ch == 'G') { printf("%s", p); return; }
        }
    }
}

/* ─── Tab completion ──────────────────────────────────────── */

static char *cmd_generator(const char *text, int state) {
    static const char *cmds[] = {
        "show", "set", "unset", "commit", "validate",
        "discard", "lock", "unlock", "dump", "help", "exit", NULL
    };
    static int idx;
    if (!state) idx = 0;
    while (cmds[idx]) {
        const char *c = cmds[idx++];
        if (strncasecmp(c, text, strlen(text)) == 0)
            return strdup(c);
    }
    return NULL;
}

static char *path_generator(const char *text, int state) {
    if (!g_schema) return NULL;
    /* Simple top-level child completion */
    static schema_node_t *cur;
    if (!state) cur = g_schema->children;
    while (cur) {
        schema_node_t *n = cur;
        cur = cur->next;
        if (strncasecmp(n->name, text, strlen(text)) == 0)
            return strdup(n->name);
    }
    return NULL;
}

static char **maapi_completer(const char *text, int start, int end) {
    (void)end;
    rl_attempted_completion_over = 1;
    /* First word: command */
    if (start == 0) return rl_completion_matches(text, cmd_generator);
    /* After show / set / unset: schema path */
    return rl_completion_matches(text, path_generator);
}

/* ─── Command handlers ────────────────────────────────────── */

static void cmd_show(char **args, int argc) {
    if (argc == 0) {
        printf("Usage: show running-config | candidate-config [path...]\n");
        return;
    }

    int db;
    if (strcasecmp(args[0], "running-config") == 0)       db = CONFD_RUNNING;
    else if (strcasecmp(args[0], "candidate-config") == 0) db = CONFD_CANDIDATE;
    else {
        printf("Unknown: %s\n", args[0]);
        return;
    }

    struct timeval t0; gettimeofday(&t0, NULL);
    char *xml = maapi_get_config_xml(g_maapi, db);
    long ms = elapsed_ms(&t0);

    if (!xml) {
        fprintf(stderr, "%sget-config failed%s\n", COLOR_RED, COLOR_RESET);
        return;
    }

    const char **path = (const char **)(args + 1);
    int path_len = argc - 1;

    char *text = fmt_xml_to_text(xml, path, path_len);
    free(xml);
    paged_print(text);
    free(text);
    printf("%s(%ldms)%s\n", COLOR_DIM, ms, COLOR_RESET);
}

/* Paste XML block cho đến dòng trống */
static char *read_xml_paste(void) {
    printf("Paste XML config (empty line to finish):\n");
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';

    char *line;
    while ((line = readline("")) != NULL) {
        if (*line == '\0') { free(line); break; }
        size_t ll = strlen(line) + 1;
        while (len + ll + 2 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(line); free(buf); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, line, ll - 1);
        len += ll - 1;
        buf[len++] = '\n';
        buf[len] = '\0';
        free(line);
    }
    return buf;
}

static void cmd_set(char **args, int argc) {
    if (argc == 0) {
        /* Paste XML */
        char *xml = read_xml_paste();
        if (!xml) return;
        if (maapi_load_xml(g_maapi, xml) == 0)
            printf("%sOK%s (staged in candidate)\n", COLOR_GREEN, COLOR_RESET);
        else
            fprintf(stderr, "%sload failed%s\n", COLOR_RED, COLOR_RESET);
        free(xml);
        return;
    }

    /*
     * Hai cú pháp:
     *   set system ntp enabled true          ← space-separated (như Go)
     *   set /system/ntp/enabled true         ← keypath ConfD (legacy)
     */
    const char *value = NULL;
    char *keypath = NULL;

    if (args[0][0] == '/') {
        /* Keypath ConfD truyền thẳng */
        if (argc < 2) {
            printf("Usage: set <path...> <value>\n"
                   "Example: set system ntp enabled true\n"
                   "         set system ntp server 10.0.0.1 prefer true\n");
            return;
        }
        keypath = strdup(args[0]);
        value   = args[1];
    } else {
        /* Space-separated: chuyển đổi qua schema */
        int consumed = 0;
        keypath = args_to_keypath(g_schema, args, argc, &consumed);
        if (!keypath || consumed == 0) {
            fprintf(stderr, "%sPath not found in schema%s\n",
                    COLOR_RED, COLOR_RESET);
            free(keypath);
            return;
        }
        if (consumed >= argc) {
            printf("Usage: set <path...> <value>\n"
                   "Example: set system hostname new-name\n");
            free(keypath);
            return;
        }
        value = args[consumed];
    }

    if (maapi_set_value_str(g_maapi, keypath, value) == 0)
        printf("%sOK%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sset failed%s\n", COLOR_RED, COLOR_RESET);
    free(keypath);
}

static void cmd_unset(char **args, int argc) {
    if (argc == 0) {
        printf("Usage: unset <path...>\n"
               "Example: unset system ntp server 10.0.0.1\n"
               "         unset /system/ntp/server{10.0.0.1}  (keypath)\n");
        return;
    }

    char *keypath = NULL;
    if (args[0][0] == '/') {
        keypath = strdup(args[0]);
    } else {
        int consumed = 0;
        keypath = args_to_keypath(g_schema, args, argc, &consumed);
        if (!keypath || consumed == 0) {
            fprintf(stderr, "%sPath not found in schema%s\n",
                    COLOR_RED, COLOR_RESET);
            free(keypath);
            return;
        }
    }

    if (maapi_delete_node(g_maapi, keypath) == 0)
        printf("%sOK%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sunset failed%s\n", COLOR_RED, COLOR_RESET);
    free(keypath);
}

static void cmd_commit(void) {
    struct timeval t0; gettimeofday(&t0, NULL);
    if (maapi_do_commit(g_maapi) == 0)
        printf("%sCommit successful.%s (%ldms)\n",
               COLOR_GREEN, COLOR_RESET, elapsed_ms(&t0));
    else
        fprintf(stderr, "%sCommit failed.%s\n", COLOR_RED, COLOR_RESET);
}

static void cmd_validate(void) {
    if (maapi_do_validate(g_maapi) == 0)
        printf("%sValidation OK.%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sValidation failed.%s\n", COLOR_RED, COLOR_RESET);
}

static void cmd_discard(void) {
    if (maapi_do_discard(g_maapi) == 0)
        printf("%sDiscarded.%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sDiscard failed.%s\n", COLOR_RED, COLOR_RESET);
}

static void cmd_lock(char **args, int argc) {
    int db = CONFD_CANDIDATE;
    if (argc > 0 && strcasecmp(args[0], "running") == 0) db = CONFD_RUNNING;
    if (maapi_do_lock(g_maapi, db) == 0)
        printf("%sLocked.%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sLock failed.%s\n", COLOR_RED, COLOR_RESET);
}

static void cmd_unlock(char **args, int argc) {
    int db = CONFD_CANDIDATE;
    if (argc > 0 && strcasecmp(args[0], "running") == 0) db = CONFD_RUNNING;
    if (maapi_do_unlock(g_maapi, db) == 0)
        printf("%sUnlocked.%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sUnlock failed.%s\n", COLOR_RED, COLOR_RESET);
}

static void cmd_dump(char **args, int argc) {
    const char *fmt  = (argc > 0) ? args[0] : "text";
    const char *file = (argc > 1) ? args[1] : NULL;

    char *xml  = maapi_get_config_xml(g_maapi, CONFD_RUNNING);
    if (!xml) { fprintf(stderr, "%sdump failed%s\n", COLOR_RED, COLOR_RESET); return; }

    char *out;
    if (strcasecmp(fmt, "xml") == 0) {
        out = xstrdup(xml);
    } else {
        out = fmt_xml_to_text(xml, NULL, 0);
    }
    free(xml);
    if (!out) return;

    if (file && *file) {
        FILE *f = fopen(file, "w");
        if (!f) {
            fprintf(stderr, "%sCannot open %s%s\n", COLOR_RED, file, COLOR_RESET);
        } else {
            fputs(out, f);
            fclose(f);
            printf("Saved to %s%s%s\n", COLOR_CYAN, file, COLOR_RESET);
        }
    } else {
        paged_print(out);
    }
    free(out);
}

static void cmd_help(void) {
    printf(
        "\n%sMAAPI Direct Mode Commands%s\n"
        "──────────────────────────────────────────────────────────\n"
        "  show running-config [path...]     Get running config\n"
        "  show candidate-config [path...]   Get candidate config\n"
        "\n"
        "  set <path...> <value>             Set leaf (space-separated)\n"
        "  set                               Paste XML config block\n"
        "\n"
        "  unset <path...>                   Delete node (space-separated)\n"
        "\n"
        "  commit                            Commit candidate → running\n"
        "  validate                          Validate candidate\n"
        "  discard                           Reset candidate to running\n"
        "  lock [running|candidate]          Lock datastore\n"
        "  unlock [running|candidate]        Unlock datastore\n"
        "  dump text [file]                  Export config as text\n"
        "  dump xml  [file]                  Export config as XML\n"
        "  help                              This message\n"
        "  exit                              Quit\n"
        "\n"
        "%sPath examples (space-separated, giống Go):%s\n"
        "  set system hostname new-host\n"
        "  set system ntp enabled true\n"
        "  set system ntp server 10.0.0.1 prefer true    ← key list\n"
        "  unset system ntp server 10.0.0.1\n"
        "  show running-config system ntp\n"
        "\n"
        "%sKeypath ConfD cũng được chấp nhận:%s\n"
        "  set /system/hostname new-host\n"
        "  unset /system/ntp/server{10.0.0.1}\n"
        "\n"
        "  %sTab completion%s: commands + top-level schema nodes.\n\n",
        COLOR_BOLD, COLOR_RESET,
        COLOR_CYAN, COLOR_RESET,
        COLOR_CYAN, COLOR_RESET,
        COLOR_CYAN, COLOR_RESET);
}

/* ─── Dispatch ────────────────────────────────────────────── */

static void dispatch(char *line) {
    int    argc = 0;
    char **argv = str_split(line, &argc);
    if (!argc) { free_tokens(argv, argc); return; }

    if      (strcasecmp(argv[0], "show")     == 0) cmd_show    (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "set")      == 0) cmd_set     (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "unset")    == 0) cmd_unset   (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "commit")   == 0) cmd_commit  ();
    else if (strcasecmp(argv[0], "validate") == 0) cmd_validate();
    else if (strcasecmp(argv[0], "discard")  == 0) cmd_discard ();
    else if (strcasecmp(argv[0], "lock")     == 0) cmd_lock    (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "unlock")   == 0) cmd_unlock  (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "dump")     == 0) cmd_dump    (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "help")     == 0) cmd_help    ();
    else if (strcasecmp(argv[0], "?")        == 0) cmd_help    ();
    else
        printf("%sUnknown command: %s%s  (type 'help')\n",
               COLOR_RED, argv[0], COLOR_RESET);

    free_tokens(argv, argc);
}

/* ─── Main ────────────────────────────────────────────────── */

int main(void) {
    signal(SIGINT, sigint_handler);
    rl_catch_signals = 0;
    rl_attempted_completion_function = maapi_completer;
    rl_variable_bind("expand-tilde", "off");

    const char *host = env_or("CONFD_IPC_ADDR", "127.0.0.1");
    int         port = env_int_or("CONFD_IPC_PORT", CONFD_PORT);
    const char *user = env_or("MAAPI_USER", "admin");
    strncpy(g_ne_name, env_or("NE_NAME", "confd"), sizeof(g_ne_name) - 1);

    printf("%s", COLOR_GREEN);
    printf("=====================================================\n");
    printf("   CLI - NETCONF Console (C / MAAPI Direct Mode)\n");
    printf("=====================================================%s\n\n", COLOR_RESET);
    printf("Connecting to ConfD MAAPI %s%s:%d%s ...\n",
           COLOR_BOLD, host, port, COLOR_RESET);

    g_maapi = maapi_dial(host, port, user);
    if (!g_maapi) {
        fprintf(stderr, "%sMAAPI connect failed. Is ConfD running?%s\n",
                COLOR_RED, COLOR_RESET);
        return 1;
    }

    printf("%sConnected.%s\n", COLOR_GREEN, COLOR_RESET);

    /* Load schema */
    printf("Loading schema from MAAPI...\n");
    g_schema = schema_new_node("__root__");
    if (g_schema) {
        maapi_load_schema_into(g_maapi, &g_schema);
        printf("Schema loaded.\n");
    }

    update_prompt();
    using_history();
    stifle_history(HISTORY_SIZE);

    printf("Type %shelp%s for commands. Ctrl+D or %sexit%s to quit.\n\n",
           COLOR_CYAN, COLOR_RESET, COLOR_CYAN, COLOR_RESET);

    /* ── Command loop ── */
    char *line;
    while ((line = readline(g_prompt)) != NULL) {
        char *trimmed = str_trim(line);
        if (*trimmed) {
            add_history(trimmed);
            if (strcasecmp(trimmed, "exit") == 0 ||
                strcasecmp(trimmed, "quit") == 0) {
                free(line);
                break;
            }
            dispatch(trimmed);
        }
        free(line);
    }

    printf("Goodbye.\n");
    schema_free(g_schema);
    cli_session_close(g_maapi);
    return 0;
}
