/*
 * completion_util.c — Pure helpers cho tab-completion.
 * See include/completion_util.h for contract.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "completion_util.h"

/* Duplicate của str_split — tách trong file riêng để tránh phụ thuộc
 * vào extern khi unit-test (schema.o không có str_split, main.o có). */
static char **split_ws(const char *s, int *count_out) {
    *count_out = 0;
    if (!s) return NULL;
    int cap = 8;
    char **arr = malloc((size_t)cap * sizeof(char *));
    if (!arr) return NULL;
    int n = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);
        if (n >= cap) {
            cap *= 2;
            char **na = realloc(arr, (size_t)cap * sizeof(char *));
            if (!na) { for (int i = 0; i < n; i++) free(arr[i]); free(arr); return NULL; }
            arr = na;
        }
        char *tok = malloc(len + 1);
        if (!tok) { for (int i = 0; i < n; i++) free(arr[i]); free(arr); return NULL; }
        memcpy(tok, start, len);
        tok[len] = '\0';
        arr[n++] = tok;
    }
    *count_out = n;
    return arr;
}

static void free_split(char **arr, int n) {
    if (!arr) return;
    for (int i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

schema_node_t *completion_parent_for(schema_node_t *schema,
                                     const char *line,
                                     int cursor) {
    if (!schema) return NULL;
    if (!line)   return schema;

    int len  = (int)strlen(line);
    int stop = (cursor >= 0 && cursor <= len) ? cursor : len;

    /* Copy phần bên trái cursor. */
    char *prefix = malloc((size_t)stop + 1);
    if (!prefix) return schema;
    memcpy(prefix, line, (size_t)stop);
    prefix[stop] = '\0';

    int count = 0;
    char **tokens = split_ws(prefix, &count);
    if (!tokens || count == 0) {
        free_split(tokens, count);
        free(prefix);
        return schema;
    }

    /* Bỏ qua token đầu (command). "show" bỏ thêm subcommand. */
    int start_idx = 1;
    if (count > 1 && strcasecmp(tokens[0], "show") == 0) {
        start_idx = 2;
    }

    /* Nếu prefix không kết thúc bằng space → token cuối là partial → bỏ. */
    int end_idx = count;
    if (stop > 0 && prefix[stop - 1] != ' ' && prefix[stop - 1] != '\t') {
        end_idx = count - 1;
    }

    schema_node_t *node = schema;
    for (int i = start_idx; i < end_idx && node; i++) {
        schema_node_t *found = NULL;
        for (schema_node_t *c = node->children; c; c = c->next) {
            if (strcasecmp(c->name, tokens[i]) == 0) { found = c; break; }
        }
        if (found) {
            node = found;
            /* Sau tên list là giá trị key — bỏ qua token tiếp theo. */
            if (found->is_list && i + 1 < end_idx) i++;
        } else {
            /* Token lạ → có thể là key value của list → dừng walk, giữ node. */
            break;
        }
    }

    free_split(tokens, count);
    free(prefix);
    return node;
}
