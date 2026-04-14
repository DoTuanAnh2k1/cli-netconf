/*
 * completer.c — Tab completion dùng GNU readline
 *
 * readline callback approach:
 *   rl_attempted_completion_function → cli_completer
 *   cli_completer parse line hiện tại để biết đang ở đâu trong path,
 *   tra schema tree và trả về danh sách matches.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "cli.h"

/* Session hiện tại (set bởi completer_update_session trước readline loop) */
static cli_session_t *g_session = NULL;

/* Commands ở top level */
static const char *BASE_CMDS[] = {
    "connect", "exit", "help", "show", NULL
};

static const char *CONNECTED_CMDS[] = {
    "commit", "connect", "discard", "disconnect",
    "dump", "exit", "help", "lock", "restore", "rpc",
    "set", "show", "unlock", "unset", "validate", NULL
};

static const char *SHOW_SUBCMDS[] = {
    "backups", "candidate-config", "ne", "running-config", NULL
};

/* -------------------------------------------------------------------------
 * Match helper — tìm entries trong list có prefix là text
 * ---------------------------------------------------------------------- */
typedef struct {
    const char **list;     /* NULL-terminated static list */
    char       **dyn_list; /* dynamic list từ schema */
    int          dyn_count;
    int          idx;
    const char  *text;
} match_state_t;

static char *match_from_list(const char **list, const char *text,
                              int state, int *s_idx) {
    if (state == 0) *s_idx = 0;
    size_t len = strlen(text);
    while (list[*s_idx]) {
        const char *entry = list[(*s_idx)++];
        if (strncasecmp(entry, text, len) == 0)
            return strdup(entry);
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Schema-based path completion
 * pathArgs = ["smf", "nsmfPduSession", ""] (last elem = partial word)
 * ---------------------------------------------------------------------- */
static char **schema_path_completions(const char **path_args, int path_count,
                                       const char *word) {
    cli_session_t *s = g_session;
    if (!s || !s->schema) return NULL;

    /* Navigate đến parent node (bỏ qua element cuối vì đó là partial) */
    const char *parent_path[MAX_PATH_DEPTH];
    int parent_depth = path_count - 1;
    if (parent_depth < 0) parent_depth = 0;
    for (int i = 0; i < parent_depth; i++) parent_path[i] = path_args[i];

    schema_node_t *node = schema_lookup(s->schema,
                                         parent_path, parent_depth);
    if (!node) return NULL;

    /* Đếm matching children */
    int match_count = 0;
    size_t word_len = strlen(word);
    for (schema_node_t *c = node->children; c; c = c->next) {
        if (strncasecmp(c->name, word, word_len) == 0) match_count++;
    }
    if (match_count == 0) return NULL;

    char **matches = calloc(match_count + 1, sizeof(char *));
    if (!matches) return NULL;

    int i = 0;
    for (schema_node_t *c = node->children; c; c = c->next) {
        if (strncasecmp(c->name, word, word_len) == 0)
            matches[i++] = strdup(c->name);
    }
    return matches;
}

/* -------------------------------------------------------------------------
 * Main completion function
 * ---------------------------------------------------------------------- */
static char **cli_completer(const char *text, int start, int end) {
    (void)end;

    cli_session_t *s = g_session;
    rl_attempted_completion_over = 1; /* Tắt default file completion */

    /* Lấy phần trước text (context) */
    char context[MAX_LINE_LEN];
    strncpy(context, rl_line_buffer, (size_t)start);
    context[start] = '\0';

    /* Tách context thành tokens */
    int   tok_count = 0;
    char **toks     = str_split(context, &tok_count);
    bool  trailing  = (start > 0 && rl_line_buffer[start - 1] == ' ');

    char **matches = NULL;

    if (tok_count == 0 || (tok_count == 1 && !trailing)) {
        /* Completing command name */
        const char **cmds = (s && s->nc) ? CONNECTED_CMDS : BASE_CMDS;
        size_t len = strlen(text);
        int n = 0;
        for (int i = 0; cmds[i]; i++)
            if (strncasecmp(cmds[i], text, len) == 0) n++;
        if (n > 0) {
            matches = calloc(n + 1, sizeof(char *));
            int j = 0;
            for (int i = 0; cmds[i]; i++)
                if (strncasecmp(cmds[i], text, len) == 0)
                    matches[j++] = strdup(cmds[i]);
        }
        goto done;
    }

    {
        const char *cmd = toks[0];

        if (strcasecmp(cmd, "show") == 0) {
            if (!trailing && tok_count == 1) goto done;
            if (tok_count == 1 && trailing) {
                /* show <TAB> → subcommands */
                size_t len = strlen(text);
                int n = 0;
                for (int i = 0; SHOW_SUBCMDS[i]; i++)
                    if (strncasecmp(SHOW_SUBCMDS[i], text, len) == 0) n++;
                if (n > 0) {
                    matches = calloc(n + 1, sizeof(char *));
                    int j = 0;
                    for (int i = 0; SHOW_SUBCMDS[i]; i++)
                        if (strncasecmp(SHOW_SUBCMDS[i], text, len) == 0)
                            matches[j++] = strdup(SHOW_SUBCMDS[i]);
                }
                goto done;
            }
            if (tok_count >= 2) {
                const char *sub = toks[1];
                if (strcasecmp(sub, "running-config") == 0 ||
                    strcasecmp(sub, "candidate-config") == 0) {
                    /* show running-config [path...] <TAB> */
                    const char *path_args[MAX_PATH_DEPTH];
                    int path_depth = 0;
                    for (int i = 2; i < tok_count && path_depth < MAX_PATH_DEPTH; i++)
                        path_args[path_depth++] = toks[i];
                    if (trailing) path_args[path_depth++] = text;
                    else if (path_depth > 0) {
                        /* replace last with text */
                        path_args[path_depth - 1] = text;
                    }
                    matches = schema_path_completions(path_args, path_depth, text);
                } else if (strcasecmp(sub, "backups") == 0) {
                    goto done;
                } else if (!trailing && tok_count == 2) {
                    /* Completing subcommand */
                    size_t len = strlen(text);
                    int n = 0;
                    for (int i = 0; SHOW_SUBCMDS[i]; i++)
                        if (strncasecmp(SHOW_SUBCMDS[i], text, len) == 0) n++;
                    if (n > 0) {
                        matches = calloc(n + 1, sizeof(char *));
                        int j = 0;
                        for (int i = 0; SHOW_SUBCMDS[i]; i++)
                            if (strncasecmp(SHOW_SUBCMDS[i], text, len) == 0)
                                matches[j++] = strdup(SHOW_SUBCMDS[i]);
                    }
                }
            }
        }

        else if (strcasecmp(cmd, "set") == 0 ||
                 strcasecmp(cmd, "unset") == 0) {
            /* set [path...] <TAB> */
            const char *path_args[MAX_PATH_DEPTH];
            int path_depth = 0;
            for (int i = 1; i < tok_count && path_depth < MAX_PATH_DEPTH; i++)
                path_args[path_depth++] = toks[i];
            if (trailing) path_args[path_depth++] = text;
            matches = schema_path_completions(path_args, path_depth, text);
        }

        else if (strcasecmp(cmd, "connect") == 0) {
            if (!s) goto done;
            size_t len = strlen(text);
            int n = 0;
            for (int i = 0; i < s->ne_count; i++) {
                if (strncasecmp(s->nes[i].name, text, len) == 0) n++;
            }
            if (n > 0) {
                matches = calloc(n + 2, sizeof(char *));
                int j = 0;
                for (int i = 0; i < s->ne_count; i++) {
                    if (strncasecmp(s->nes[i].name, text, len) == 0)
                        matches[j++] = strdup(s->nes[i].name);
                }
            }
        }

        else if (strcasecmp(cmd, "dump") == 0) {
            if (tok_count == 1 && trailing) {
                const char *fmts[] = {"text", "xml", NULL};
                size_t len = strlen(text);
                int n = 0;
                for (int i = 0; fmts[i]; i++)
                    if (strncasecmp(fmts[i], text, len) == 0) n++;
                if (n > 0) {
                    matches = calloc(n + 1, sizeof(char *));
                    int j = 0;
                    for (int i = 0; fmts[i]; i++)
                        if (strncasecmp(fmts[i], text, len) == 0)
                            matches[j++] = strdup(fmts[i]);
                }
            }
        }

        else if (strcasecmp(cmd, "lock") == 0 ||
                 strcasecmp(cmd, "unlock") == 0) {
            const char *ds[] = {"candidate", "running", NULL};
            size_t len = strlen(text);
            int n = 0;
            for (int i = 0; ds[i]; i++)
                if (strncasecmp(ds[i], text, len) == 0) n++;
            if (n > 0) {
                matches = calloc(n + 1, sizeof(char *));
                int j = 0;
                for (int i = 0; ds[i]; i++)
                    if (strncasecmp(ds[i], text, len) == 0)
                        matches[j++] = strdup(ds[i]);
            }
        }

        else if (strcasecmp(cmd, "restore") == 0) {
            if (!s) goto done;
            size_t len = strlen(text);
            int n = 0;
            for (int i = 0; i < s->backup_count; i++) {
                char id_str[16];
                snprintf(id_str, sizeof(id_str), "%d", s->backups[i].id);
                if (strncmp(id_str, text, len) == 0) n++;
            }
            if (n > 0) {
                matches = calloc(n + 1, sizeof(char *));
                int j = 0;
                for (int i = 0; i < s->backup_count; i++) {
                    char id_str[16];
                    snprintf(id_str, sizeof(id_str), "%d", s->backups[i].id);
                    if (strncmp(id_str, text, len) == 0)
                        matches[j++] = strdup(id_str);
                }
            }
        }
    }

done:
    free_tokens(toks, tok_count);
    return matches;
}

/* -------------------------------------------------------------------------
 * completer_init — gọi một lần khi khởi động readline
 * ---------------------------------------------------------------------- */
void completer_init(cli_session_t *s) {
    g_session = s;
    rl_attempted_completion_function = cli_completer;
    /* Disable ! history expansion */
    rl_variable_bind("expand-tilde", "off");
    /* Readline không thêm space sau khi complete thành công */
    rl_completion_append_character = ' ';
}

/* -------------------------------------------------------------------------
 * completer_update_session — gọi mỗi khi connect/disconnect NE
 * ---------------------------------------------------------------------- */
void completer_update_session(cli_session_t *s) {
    g_session = s;
}
