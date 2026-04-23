/*
 * cli.h — shared types, constants, and forward declarations
 *         for the MAAPI-based CLI written in C
 */
#ifndef CLI_H
#define CLI_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */
#define MAX_PATH_DEPTH   32
#define MAX_NAME_LEN     128
#define MAX_NS_LEN       512
#define MAX_LINE_LEN     4096
#define MAX_TOKEN_LEN    1024
#define MAX_BUF          (1024 * 1024)   /* 1 MB XML buffer */
#define PAGE_SIZE        20
#define HISTORY_SIZE     100

/* -------------------------------------------------------------------------
 * Schema node — schema tree used for tab completion
 * Built from YANG via MAAPI load_schema.
 * ---------------------------------------------------------------------- */
/* Số key tối đa của 1 YANG list — đủ cho compound key thực tế (ConfD dùng 9). */
#define MAX_LIST_KEYS  8

typedef struct schema_node {
    char              name[MAX_NAME_LEN];
    char              ns[MAX_NS_LEN];
    bool              is_list;     /* YANG list (has key) */
    bool              is_leaf;     /* leaf or leaf-list */
    /* Tên các key leaf — chỉ có nghĩa khi is_list = true. n_keys = 0 nếu
     * không phải list. Dùng để:
     *   - ẩn key khỏi gợi ý tab completion sau khi user đã nhập key value
     *   - validate batch set không cho set lại key leaf */
    char              keys[MAX_LIST_KEYS][MAX_NAME_LEN];
    int               n_keys;
    struct schema_node *children;  /* child list (linked list) */
    struct schema_node *next;      /* next sibling */
} schema_node_t;

/* Kiểm tra xem `leaf_name` có phải là 1 trong các key của list `list_node`
 * (trả false nếu list_node không phải list hoặc không có key). */
bool schema_is_key_leaf(const schema_node_t *list_node, const char *leaf_name);

/* -------------------------------------------------------------------------
 * Forward declarations — schema.c
 * ---------------------------------------------------------------------- */
schema_node_t *schema_new_node(const char *name);
void           schema_free(schema_node_t *root);
schema_node_t *schema_lookup(schema_node_t *root, const char **path, int depth);
void           schema_merge(schema_node_t *dst, schema_node_t *src);
schema_node_t *schema_parse_xml(const char *xml_data);
schema_node_t *schema_parse_yang(const char *yang_text, const char *ns);
char         **schema_child_names(schema_node_t *node, int *count);

/* -------------------------------------------------------------------------
 * Forward declarations — formatter.c
 * ---------------------------------------------------------------------- */
char *fmt_xml_to_text(const char *xml_data, const char **path, int path_len);
char *fmt_extract_data_xml(const char *rpc_reply);
char *fmt_extract_raw_data(const char *rpc_reply);
char *fmt_text_to_xml(const char *text, schema_node_t *schema);
bool  fmt_is_rpc_ok(const char *reply);
bool  fmt_is_rpc_error(const char *reply);
char *fmt_extract_error_msg(const char *reply);

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Split string into token array (caller free with free_tokens()) */
char **str_split(const char *s, int *count);
void   free_tokens(char **tokens, int count);

/* Trim leading/trailing whitespace in-place */
char *str_trim(char *s);

/* Safe strdup */
char *xstrdup(const char *s);

/* ANSI color macros */
#define COLOR_RED    "\033[31m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_CYAN   "\033[36m"
#define COLOR_WHITE  "\033[97m"
#define COLOR_BOLD   "\033[1m"
#define COLOR_DIM    "\033[90m"
#define COLOR_RESET  "\033[0m"

#endif /* CLI_H */
