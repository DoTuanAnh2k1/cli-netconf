/*
 * cli.h — shared types, constants và forward declarations
 *         cho CLI NETCONF viết bằng C + MAAPI
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
#define MAX_URL_LEN      512
#define MAX_NE           64
#define MAX_BUF          (1024 * 1024)   /* 1 MB XML buffer */
#define NETCONF_DELIM    "]]>]]>"
#define NETCONF_DELIM_LEN 6
#define PAGE_SIZE        20
#define HISTORY_SIZE     100

/* -------------------------------------------------------------------------
 * Schema node — cây schema dùng cho tab completion
 * Được build từ YANG (qua NETCONF get-schema hoặc MAAPI)
 * hoặc từ XML running-config nếu không có YANG.
 * ---------------------------------------------------------------------- */
typedef struct schema_node {
    char              name[MAX_NAME_LEN];
    char              ns[MAX_NS_LEN];
    bool              is_list;     /* YANG list (có key) */
    bool              is_leaf;     /* leaf hoặc leaf-list */
    struct schema_node *children;  /* danh sách con (linked list) */
    struct schema_node *next;      /* sibling tiếp theo */
} schema_node_t;

/* -------------------------------------------------------------------------
 * NE info — lấy từ mgt-service
 * ---------------------------------------------------------------------- */
typedef struct ne_info {
    char name[MAX_NAME_LEN];
    char site[MAX_NAME_LEN];
    char ip[64];
    int  port;
    char ns[MAX_NS_LEN];
    char description[256];
} ne_info_t;

/* -------------------------------------------------------------------------
 * Backup snapshot
 * ---------------------------------------------------------------------- */
typedef struct backup {
    int   id;
    int   remote_id;
    char  timestamp[64];
    char *xml;           /* NULL nếu chưa fetch (lazy) */
} backup_t;

/* -------------------------------------------------------------------------
 * NETCONF session — kết nối tới một NE
 * ---------------------------------------------------------------------- */
typedef struct netconf_session {
    int    fd;                /* TCP socket fd hoặc -1 nếu dùng SSH */
    void  *ssh_session;       /* libssh2 session (nếu SSH mode) */
    void  *ssh_channel;       /* libssh2 channel  */
    uint64_t msg_id;          /* NETCONF message-id tự tăng */
    char   session_id[64];    /* server session-id từ HELLO */
    char **capabilities;      /* server capabilities (NULL-terminated) */
    int    cap_count;
} netconf_session_t;

/* -------------------------------------------------------------------------
 * CLI session — trạng thái toàn bộ một phiên làm việc
 * ---------------------------------------------------------------------- */
typedef struct cli_session {
    /* Auth */
    char token[MAX_TOKEN_LEN];
    char username[MAX_NAME_LEN];

    /* mgt-service */
    char mgt_url[MAX_URL_LEN];

    /* Danh sách NE */
    ne_info_t nes[MAX_NE];
    int       ne_count;

    /* NE hiện tại đang kết nối */
    ne_info_t        *current_ne;
    netconf_session_t *nc;

    /* Schema cho tab completion */
    schema_node_t *schema;

    /* Backup list */
    backup_t *backups;
    int       backup_count;
    int       backup_seq;

    /* Terminal prompt */
    char prompt[256];
} cli_session_t;

/* -------------------------------------------------------------------------
 * Forward declarations — auth.c
 * ---------------------------------------------------------------------- */
bool auth_login(cli_session_t *s, const char *username, const char *password);
bool auth_list_ne(cli_session_t *s);
void auth_save_history(cli_session_t *s, const char *cmd, long ms);
bool auth_save_backup(cli_session_t *s, const char *xml, int *out_remote_id);
bool auth_list_backups(cli_session_t *s);
bool auth_get_backup(cli_session_t *s, int remote_id, char **out_xml);

/* -------------------------------------------------------------------------
 * Forward declarations — netconf.c
 * ---------------------------------------------------------------------- */
netconf_session_t *nc_dial_tcp(const char *host, int port);
netconf_session_t *nc_dial_ssh(const char *host, int port,
                               const char *user, const char *pass);
void               nc_close(netconf_session_t *nc);
char              *nc_get_config(netconf_session_t *nc, const char *datastore,
                                 const char *filter);
char              *nc_edit_config(netconf_session_t *nc, const char *datastore,
                                  const char *xml);
char              *nc_commit(netconf_session_t *nc);
char              *nc_validate(netconf_session_t *nc, const char *datastore);
char              *nc_discard(netconf_session_t *nc);
char              *nc_lock(netconf_session_t *nc, const char *datastore);
char              *nc_unlock(netconf_session_t *nc, const char *datastore);
char              *nc_copy_config(netconf_session_t *nc, const char *target,
                                  const char *xml);
char              *nc_get_schema(netconf_session_t *nc, const char *identifier);
char              *nc_send_rpc(netconf_session_t *nc, const char *body);
char             **nc_extract_modules(netconf_session_t *nc, int *count);

/* -------------------------------------------------------------------------
 * Forward declarations — schema.c
 * ---------------------------------------------------------------------- */
schema_node_t *schema_new_node(const char *name);
void           schema_free(schema_node_t *root);
schema_node_t *schema_lookup(schema_node_t *root, const char **path, int depth);
void           schema_merge(schema_node_t *dst, schema_node_t *src);
schema_node_t *schema_parse_xml(const char *xml_data);
schema_node_t *schema_parse_yang(const char *yang_text, const char *ns);
void           schema_load(cli_session_t *s);
/* Trả về danh sách tên con (caller free) */
char         **schema_child_names(schema_node_t *node, int *count);

/* -------------------------------------------------------------------------
 * Forward declarations — maapi.c  (chỉ compile khi có WITH_MAAPI)
 * ---------------------------------------------------------------------- */
#ifdef WITH_MAAPI
bool           maapi_load_schema(cli_session_t *s);
#endif

/* -------------------------------------------------------------------------
 * Forward declarations — completer.c
 * ---------------------------------------------------------------------- */
void completer_init(cli_session_t *s);
void completer_update_session(cli_session_t *s);

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
 * Forward declarations — commands.c
 * ---------------------------------------------------------------------- */
void cmd_dispatch(cli_session_t *s, const char *line);

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Tách string thành mảng token (caller free với free_tokens()) */
char **str_split(const char *s, int *count);
void   free_tokens(char **tokens, int count);

/* Trim whitespace */
char *str_trim(char *s);

/* strdup an toàn */
char *xstrdup(const char *s);

/* printf màu ANSI */
#define COLOR_RED    "\033[31m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_CYAN   "\033[36m"
#define COLOR_BOLD   "\033[1m"
#define COLOR_DIM    "\033[90m"
#define COLOR_RESET  "\033[0m"

#endif /* CLI_H */
