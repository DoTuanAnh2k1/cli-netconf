/*
 * maapi-direct.h — MAAPI session types cho direct mode
 *
 * Build: make WITH_MAAPI=1 CONFD_DIR=/path/to/confd maapi-direct
 *
 * MAAPI (Management Agent API) là IPC nội bộ ConfD (:4565).
 * Toàn bộ read/write config đi qua MAAPI — không cần NETCONF session.
 *
 *  Env vars:
 *    CONFD_IPC_ADDR   ConfD host    (default: 127.0.0.1)
 *    CONFD_IPC_PORT   ConfD port    (default: 4565)
 *    MAAPI_USER       user session  (default: admin)
 */
#ifndef MAAPI_DIRECT_H
#define MAAPI_DIRECT_H

#ifdef WITH_MAAPI

#include <stdbool.h>

/* ─── MAAPI session ─────────────────────────────────────── */
typedef struct maapi_session {
    int  sock;          /* connected MAAPI socket */
    int  th_write;      /* open write transaction (candidate) */
    bool has_write;     /* write transaction open? */
    char host[128];
    int  port;
} maapi_session_t;

/* Kết nối tới ConfD MAAPI */
maapi_session_t *maapi_dial(const char *host, int port, const char *user);

/* Đóng kết nối */
void maapi_close(maapi_session_t *m);

/* ─── Read operations ───────────────────────────────────── */

/*
 * Export config từ datastore (CONFD_RUNNING hoặc CONFD_CANDIDATE).
 * Trả về XML string đầy đủ trong envelope <rpc-reply><data>...</data></rpc-reply>
 * (cùng format với NETCONF get-config → dùng được với fmt_xml_to_text).
 * Caller phải free().
 */
char *maapi_get_config_xml(maapi_session_t *m, int db);

/* ─── Write operations ──────────────────────────────────── */

/*
 * Set một leaf theo keypath ConfD ("/system/hostname").
 * value là string, ConfD tự convert kiểu.
 * Trả về 0 OK, -1 lỗi.
 */
int maapi_set_value_str(maapi_session_t *m,
                        const char *keypath, const char *value);

/*
 * Load XML config string vào candidate (merge).
 * xml là NETCONF XML config (nội dung bên trong <config>).
 * Trả về 0 OK, -1 lỗi.
 */
int maapi_load_xml(maapi_session_t *m, const char *xml);

/*
 * Xoá node tại keypath.
 * Trả về 0 OK, -1 lỗi.
 */
int maapi_delete_node(maapi_session_t *m, const char *keypath);

/* ─── Transaction control ───────────────────────────────── */

/* Validate candidate. Trả về 0 OK, -1 lỗi. */
int maapi_do_validate(maapi_session_t *m);

/* Commit candidate → running. Trả về 0 OK, -1 lỗi. */
int maapi_do_commit(maapi_session_t *m);

/* Discard candidate (reset về running). Trả về 0 OK, -1 lỗi. */
int maapi_do_discard(maapi_session_t *m);

/* Lock/unlock datastore. db = CONFD_RUNNING | CONFD_CANDIDATE */
int maapi_do_lock(maapi_session_t *m, int db);
int maapi_do_unlock(maapi_session_t *m, int db);

/* ─── Schema ────────────────────────────────────────────── */

/* Load YANG schema từ ConfD vào schema_node tree */
#include "cli.h"
/* Low-level: connect to ConfD at host:port and walk schema into *out_schema */
bool maapi_load_schema(const char *host, int port, schema_node_t **out_schema);
/* High-level: reuse existing maapi_session connection info */
bool maapi_load_schema_into(maapi_session_t *m, schema_node_t **out_schema);

/* ─── Path conversion ───────────────────────────────────── */

/*
 * Chuyển space-separated path tokens thành ConfD keypath string.
 *
 * Input:  schema, args = ["system","ntp","server","10.0.0.1","prefer"], argc=5
 * Output: "/system/ntp/server{10.0.0.1}/prefer"  (*consumed = 5)
 *
 * Nếu node là is_list, token tiếp theo được bọc trong {} làm key.
 * Trả về malloc'd string (caller free), hoặc NULL nếu path không tìm thấy.
 * *consumed = số token đã dùng (phần còn lại = value).
 */
char *args_to_keypath(schema_node_t *schema,
                      char **args, int argc,
                      int *consumed);

#endif /* WITH_MAAPI */
#endif /* MAAPI_DIRECT_H */
