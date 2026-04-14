/*
 * libclinetconf.h — Public C API cho CLI-NETCONF library
 *
 * Build:
 *   make -C c lib          →  c/libclinetconf.a  (static)
 *   make -C c lib-shared   →  c/libclinetconf.so (dynamic)
 *
 * Usage from C:
 *   #include "libclinetconf.h"
 *   cli_handle_t *h = cli_create("http://mgt-service:3000");
 *   cli_login(h, "admin", "admin");
 *   ...
 *   cli_destroy(h);
 *
 * Usage from Go (CGo):
 *   see go/pkg/ccli/ccli.go
 */
#ifndef LIBCLINETCONF_H
#define LIBCLINETCONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque handle — ẩn nội bộ khỏi caller
 * ---------------------------------------------------------------------- */
typedef struct cli_handle cli_handle_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/* Tạo handle mới; mgt_url = "http://host:port" */
cli_handle_t *cli_create(const char *mgt_url);

/* Giải phóng toàn bộ tài nguyên */
void          cli_destroy(cli_handle_t *h);

/* -------------------------------------------------------------------------
 * Auth & NE list
 * ---------------------------------------------------------------------- */

/* Đăng nhập mgt-service; trả về 0 = OK, -1 = lỗi */
int  cli_login(cli_handle_t *h, const char *username, const char *password);

/* Lấy danh sách NE từ mgt-service; phải gọi sau cli_login */
int  cli_list_ne(cli_handle_t *h);

/* Số lượng NE */
int  cli_ne_count(cli_handle_t *h);

/* Tên NE theo index (0-based); NULL nếu out-of-range */
const char *cli_ne_name(cli_handle_t *h, int idx);

/* IP của NE theo index */
const char *cli_ne_ip(cli_handle_t *h, int idx);

/* Port của NE theo index */
int  cli_ne_port(cli_handle_t *h, int idx);

/* Site của NE theo index */
const char *cli_ne_site(cli_handle_t *h, int idx);

/* Description của NE theo index */
const char *cli_ne_description(cli_handle_t *h, int idx);

/* -------------------------------------------------------------------------
 * Connection
 * ---------------------------------------------------------------------- */

/*
 * Kết nối tới NE.
 *   ne      — tên NE (case-insensitive) hoặc số thứ tự ("1", "2", ...)
 *   mode    — "tcp" (default) hoặc "ssh"
 *   host    — override host; NULL = dùng IP từ NE list
 *   port    — override port; 0  = dùng port từ NE list
 *   nc_user — NETCONF SSH username (chỉ dùng khi mode="ssh")
 *   nc_pass — NETCONF SSH password (chỉ dùng khi mode="ssh")
 *
 * Trả về 0 = OK, -1 = lỗi
 */
int  cli_connect(cli_handle_t *h,
                 const char *ne,
                 const char *mode,
                 const char *host,   /* nullable */
                 int         port,   /* 0 = auto */
                 const char *nc_user,
                 const char *nc_pass);

/* Ngắt kết nối NETCONF hiện tại */
void cli_disconnect(cli_handle_t *h);

/* 1 nếu đang kết nối, 0 nếu không */
int  cli_is_connected(cli_handle_t *h);

/* Tên NE đang kết nối; NULL nếu chưa kết nối */
const char *cli_current_ne(cli_handle_t *h);

/* NETCONF session-id của kết nối hiện tại */
const char *cli_session_id(cli_handle_t *h);

/* -------------------------------------------------------------------------
 * Schema
 * ---------------------------------------------------------------------- */

/* Load schema (MAAPI → get-schema → XML fallback) */
void cli_load_schema(cli_handle_t *h);

/*
 * Lấy danh sách tên con của node tại path.
 *   path  — mảng tên phần tử, ví dụ {"system", "ntp"}
 *   depth — số phần tử trong path
 *   count — [out] số lượng kết quả
 *
 * Trả về mảng string (caller free từng string rồi free mảng),
 * hoặc NULL nếu không tìm thấy.
 */
char **cli_schema_children(cli_handle_t *h,
                            const char **path, int depth,
                            int *count);

/* -------------------------------------------------------------------------
 * NETCONF operations
 * Tất cả hàm trả về string đều malloc'd — caller phải free().
 * Trả về NULL nếu lỗi.
 * ---------------------------------------------------------------------- */

/*
 * get-config
 *   datastore — "running" | "candidate"
 *   filter    — subtree XML filter; NULL hoặc "" = lấy toàn bộ
 * Trả về XML string của <rpc-reply>
 */
char *cli_get_config(cli_handle_t *h,
                     const char *datastore,
                     const char *filter);

/*
 * get-config dạng text đã format, có thể filter theo path.
 *   path     — mảng path; NULL = toàn bộ
 *   path_len — 0 = toàn bộ
 * Trả về text string (indented)
 */
char *cli_get_config_text(cli_handle_t *h,
                           const char *datastore,
                           const char **path, int path_len);

/*
 * edit-config
 *   datastore — "candidate" (thường dùng)
 *   xml       — config XML cần set (nội dung bên trong <config>)
 * Trả về 0 = OK, -1 = lỗi
 */
int  cli_edit_config(cli_handle_t *h,
                     const char *datastore,
                     const char *xml);

/* commit; trả về 0 = OK, -1 = lỗi */
int  cli_commit(cli_handle_t *h);

/* validate; trả về 0 = OK, -1 = lỗi */
int  cli_validate(cli_handle_t *h);

/* discard-changes; trả về 0 = OK, -1 = lỗi */
int  cli_discard(cli_handle_t *h);

/* lock datastore; trả về 0 = OK, -1 = lỗi */
int  cli_lock(cli_handle_t *h, const char *datastore);

/* unlock datastore; trả về 0 = OK, -1 = lỗi */
int  cli_unlock(cli_handle_t *h, const char *datastore);

/* copy-config; trả về 0 = OK, -1 = lỗi */
int  cli_copy_config(cli_handle_t *h,
                     const char *target,
                     const char *source_xml);

/* Gửi RPC body tự do; trả về <rpc-reply> XML string */
char *cli_send_rpc(cli_handle_t *h, const char *body);

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/* Kiểm tra reply có <ok/> không */
int  cli_reply_is_ok(const char *rpc_reply);

/* Kiểm tra reply có <rpc-error> không */
int  cli_reply_is_error(const char *rpc_reply);

/* Lấy error-message từ reply (malloc'd, caller free); NULL nếu không có */
char *cli_reply_error_msg(const char *rpc_reply);

/* Lấy <data>...</data> từ reply (malloc'd, caller free) */
char *cli_reply_data_xml(const char *rpc_reply);

/* -------------------------------------------------------------------------
 * Interactive session (chạy full CLI như binary)
 * ---------------------------------------------------------------------- */

/* Chạy interactive CLI loop trên stdin/stdout; block cho đến khi user exit.
 * Tương đương chạy binary cli-netconf-c. */
int  cli_run_interactive(const char *mgt_url);

#ifdef __cplusplus
}
#endif

#endif /* LIBCLINETCONF_H */
