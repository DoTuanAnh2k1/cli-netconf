/*
 * lib.c — Implementation của libclinetconf public API
 *
 * struct cli_handle wraps cli_session_t (opaque với caller).
 * Mọi hàm delegate xuống các internal module (auth, netconf, schema,
 * formatter, completer).
 *
 * Build:
 *   make -C c lib           →  c/libclinetconf.a
 *   make -C c lib-shared    →  c/libclinetconf.so
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <curl/curl.h>
#include <libxml/parser.h>

#include "cli.h"
#include "libclinetconf.h"

/* ─────────────────────────────────────────────
 * Opaque handle definition
 * ──────────────────────────────────────────── */
struct cli_handle {
    cli_session_t s;
};

/* ─────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────── */

cli_handle_t *cli_create(const char *mgt_url) {
    /* Khởi tạo thư viện lần đầu (idempotent nếu gọi nhiều lần) */
    curl_global_init(CURL_GLOBAL_DEFAULT);
    xmlInitParser();

    cli_handle_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;

    strncpy(h->s.mgt_url,
            mgt_url ? mgt_url : "http://127.0.0.1:3000",
            sizeof(h->s.mgt_url) - 1);
    return h;
}

void cli_destroy(cli_handle_t *h) {
    if (!h) return;

    /* Đóng NETCONF nếu còn mở */
    if (h->s.nc) {
        nc_close(h->s.nc);
        h->s.nc = NULL;
    }

    /* Giải phóng schema */
    schema_free(h->s.schema);

    /* Giải phóng backups */
    if (h->s.backups) {
        for (int i = 0; i < h->s.backup_count; i++)
            free(h->s.backups[i].xml);
        free(h->s.backups);
    }

    free(h);
    /* Không gọi curl_global_cleanup / xmlCleanupParser ở đây vì có thể còn
       handle khác đang dùng; caller chịu trách nhiệm cleanup global nếu cần */
}

/* ─────────────────────────────────────────────
 * Auth & NE list
 * ──────────────────────────────────────────── */

int cli_login(cli_handle_t *h, const char *username, const char *password) {
    if (!h) return -1;
    if (!auth_login(&h->s, username, password)) return -1;
    strncpy(h->s.username, username ? username : "",
            sizeof(h->s.username) - 1);
    return 0;
}

int cli_list_ne(cli_handle_t *h) {
    if (!h) return -1;
    return auth_list_ne(&h->s) ? 0 : -1;
}

int cli_ne_count(cli_handle_t *h) {
    return h ? h->s.ne_count : 0;
}

const char *cli_ne_name(cli_handle_t *h, int idx) {
    if (!h || idx < 0 || idx >= h->s.ne_count) return NULL;
    return h->s.nes[idx].name;
}

const char *cli_ne_ip(cli_handle_t *h, int idx) {
    if (!h || idx < 0 || idx >= h->s.ne_count) return NULL;
    return h->s.nes[idx].ip;
}

int cli_ne_port(cli_handle_t *h, int idx) {
    if (!h || idx < 0 || idx >= h->s.ne_count) return -1;
    return h->s.nes[idx].port;
}

const char *cli_ne_site(cli_handle_t *h, int idx) {
    if (!h || idx < 0 || idx >= h->s.ne_count) return NULL;
    return h->s.nes[idx].site;
}

const char *cli_ne_description(cli_handle_t *h, int idx) {
    if (!h || idx < 0 || idx >= h->s.ne_count) return NULL;
    return h->s.nes[idx].description;
}

/* ─────────────────────────────────────────────
 * Connection
 * ──────────────────────────────────────────── */

/* Tìm index NE theo tên hoặc số thứ tự ("1", "2", ...) */
static int resolve_ne_idx(cli_session_t *s, const char *ne) {
    if (!ne) return -1;
    char *endp;
    long idx = strtol(ne, &endp, 10);
    if (*endp == '\0' && idx >= 1 && idx <= s->ne_count)
        return (int)idx - 1;
    for (int i = 0; i < s->ne_count; i++) {
        if (strcasecmp(s->nes[i].name, ne) == 0) return i;
    }
    return -1;
}

int cli_connect(cli_handle_t *h,
                const char *ne,
                const char *mode,
                const char *host,
                int port,
                const char *nc_user,
                const char *nc_pass) {
    if (!h) return -1;

    /* Resolve NE */
    int idx = resolve_ne_idx(&h->s, ne);
    if (idx < 0) {
        fprintf(stderr, "cli_connect: NE '%s' not found\n", ne ? ne : "(null)");
        return -1;
    }
    ne_info_t *ne_info = &h->s.nes[idx];

    /* Fallbacks */
    if (!mode) mode = "tcp";
    if (!host || !*host) host = ne_info->ip;
    if (port <= 0) port = ne_info->port;
    if (!nc_user || !*nc_user) nc_user = "admin";
    if (!nc_pass || !*nc_pass) nc_pass = "admin";

    netconf_session_t *nc = NULL;
    if (strcasecmp(mode, "ssh") == 0) {
#ifdef WITH_SSH
        nc = nc_dial_ssh(host, port, nc_user, nc_pass);
#else
        fprintf(stderr, "cli_connect: compiled without SSH support\n");
        return -1;
#endif
    } else {
        nc = nc_dial_tcp(host, port);
    }

    if (!nc) return -1;

    /* Đóng session cũ nếu có */
    if (h->s.nc) {
        nc_close(h->s.nc);
        schema_free(h->s.schema);
        h->s.schema = NULL;
    }

    h->s.nc         = nc;
    h->s.current_ne = ne_info;
    return 0;
}

void cli_disconnect(cli_handle_t *h) {
    if (!h || !h->s.nc) return;
    nc_close(h->s.nc);
    h->s.nc         = NULL;
    h->s.current_ne = NULL;
    schema_free(h->s.schema);
    h->s.schema = NULL;
}

int cli_is_connected(cli_handle_t *h) {
    return (h && h->s.nc) ? 1 : 0;
}

const char *cli_current_ne(cli_handle_t *h) {
    if (!h || !h->s.current_ne) return NULL;
    return h->s.current_ne->name;
}

const char *cli_session_id(cli_handle_t *h) {
    if (!h || !h->s.nc) return NULL;
    return h->s.nc->session_id;
}

/* ─────────────────────────────────────────────
 * Schema
 * ──────────────────────────────────────────── */

void cli_load_schema(cli_handle_t *h) {
    if (!h) return;
    schema_load(&h->s);
}

char **cli_schema_children(cli_handle_t *h,
                            const char **path, int depth,
                            int *count) {
    if (count) *count = 0;
    if (!h || !h->s.schema) return NULL;

    schema_node_t *node = h->s.schema;
    if (depth > 0 && path) {
        node = schema_lookup(h->s.schema, path, depth);
        if (!node) return NULL;
    }
    return schema_child_names(node, count);
}

/* ─────────────────────────────────────────────
 * NETCONF operations
 * ──────────────────────────────────────────── */

char *cli_get_config(cli_handle_t *h,
                     const char *datastore,
                     const char *filter) {
    if (!h || !h->s.nc) return NULL;
    if (!datastore) datastore = "running";
    return nc_get_config(h->s.nc, datastore, filter);
}

char *cli_get_config_text(cli_handle_t *h,
                           const char *datastore,
                           const char **path, int path_len) {
    char *reply = cli_get_config(h, datastore, NULL);
    if (!reply) return NULL;
    char *data = fmt_extract_raw_data(reply);
    free(reply);
    if (!data) return NULL;
    char *text = fmt_xml_to_text(data, path, path_len);
    free(data);
    return text;
}

int cli_edit_config(cli_handle_t *h,
                    const char *datastore,
                    const char *xml) {
    if (!h || !h->s.nc || !xml) return -1;
    if (!datastore) datastore = "candidate";
    char *reply = nc_edit_config(h->s.nc, datastore, xml);
    if (!reply) return -1;
    int ok = fmt_is_rpc_ok(reply) ? 0 : -1;
    free(reply);
    return ok;
}

int cli_commit(cli_handle_t *h) {
    if (!h || !h->s.nc) return -1;
    char *reply = nc_commit(h->s.nc);
    if (!reply) return -1;
    int ok = fmt_is_rpc_ok(reply) ? 0 : -1;
    free(reply);
    return ok;
}

int cli_validate(cli_handle_t *h) {
    if (!h || !h->s.nc) return -1;
    char *reply = nc_validate(h->s.nc, "candidate");
    if (!reply) return -1;
    int ok = fmt_is_rpc_ok(reply) ? 0 : -1;
    free(reply);
    return ok;
}

int cli_discard(cli_handle_t *h) {
    if (!h || !h->s.nc) return -1;
    char *reply = nc_discard(h->s.nc);
    if (!reply) return -1;
    int ok = fmt_is_rpc_ok(reply) ? 0 : -1;
    free(reply);
    return ok;
}

int cli_lock(cli_handle_t *h, const char *datastore) {
    if (!h || !h->s.nc) return -1;
    if (!datastore) datastore = "candidate";
    char *reply = nc_lock(h->s.nc, datastore);
    if (!reply) return -1;
    int ok = fmt_is_rpc_ok(reply) ? 0 : -1;
    free(reply);
    return ok;
}

int cli_unlock(cli_handle_t *h, const char *datastore) {
    if (!h || !h->s.nc) return -1;
    if (!datastore) datastore = "candidate";
    char *reply = nc_unlock(h->s.nc, datastore);
    if (!reply) return -1;
    int ok = fmt_is_rpc_ok(reply) ? 0 : -1;
    free(reply);
    return ok;
}

int cli_copy_config(cli_handle_t *h,
                    const char *target,
                    const char *source_xml) {
    if (!h || !h->s.nc) return -1;
    if (!target) target = "running";
    char *reply = nc_copy_config(h->s.nc, target, source_xml);
    if (!reply) return -1;
    int ok = fmt_is_rpc_ok(reply) ? 0 : -1;
    free(reply);
    return ok;
}

char *cli_send_rpc(cli_handle_t *h, const char *body) {
    if (!h || !h->s.nc || !body) return NULL;
    return nc_send_rpc(h->s.nc, body);
}

/* ─────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────── */

int cli_reply_is_ok(const char *rpc_reply) {
    return fmt_is_rpc_ok(rpc_reply) ? 1 : 0;
}

int cli_reply_is_error(const char *rpc_reply) {
    return fmt_is_rpc_error(rpc_reply) ? 1 : 0;
}

char *cli_reply_error_msg(const char *rpc_reply) {
    if (!fmt_is_rpc_error(rpc_reply)) return NULL;
    return fmt_extract_error_msg(rpc_reply);
}

char *cli_reply_data_xml(const char *rpc_reply) {
    return fmt_extract_data_xml(rpc_reply);
}

/* ─────────────────────────────────────────────
 * Interactive session
 * ──────────────────────────────────────────── */

/*
 * cli_run_interactive — khởi động full CLI loop (giống binary cli-netconf-c).
 * Gọi hàm main() logic từ main.c nhưng main.c không export được, nên ta
 * duplicate phần khởi động tối thiểu ở đây.
 *
 * Điều kiện: chỉ hoạt động đúng khi stdin/stdout là terminal.
 */
int cli_run_interactive(const char *mgt_url) {
    /* Đặt MGT_URL để main.c internal functions đọc được qua getenv */
    if (mgt_url && *mgt_url) {
        setenv("MGT_URL", mgt_url, 1);
    }

    /* Tạo process mới chạy binary cli-netconf-c nếu muốn hoàn toàn sạch.
     * Ở đây ta gọi trực tiếp bằng cách re-implement phần nhỏ của main().
     * Yêu cầu include readline — chỉ link khi build executable. */

    /* Delegate: dùng system() để chạy binary trong PATH nếu có */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "MGT_URL=%s cli-netconf-c",
             mgt_url ? mgt_url : "http://127.0.0.1:3000");
    return system(cmd);
}
