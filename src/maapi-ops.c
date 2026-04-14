/*
 * maapi-ops.c — MAAPI-based config operations
 *   get-config  → maapi_save_config (MAAPI_CONFIG_XML)
 *   edit-config → maapi_load_config (MAAPI_CONFIG_XML | MAAPI_CONFIG_MERGE)
 *   set leaf    → maapi_set_elem / maapi_set_elem2
 *   delete      → maapi_delete
 *   commit      → maapi_candidate_commit
 *   discard     → maapi_candidate_reset
 *   validate    → maapi_validate_trans
 */
#ifdef WITH_MAAPI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "confd_compat.h"
#include "cli.h"
#include "maapi-direct.h"

/* ─── Internal helpers ──────────────────────────────────── */

/* Đọc toàn bộ file vào chuỗi (malloc'd, caller free) */
static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return strdup(""); }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Tạo tên file tạm, ghi string vào, trả về path (caller unlink + free) */
static char *write_tmp_str(const char *str) {
    char *path = strdup("/tmp/maapi-XXXXXX");
    if (!path) return NULL;
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    if (str && *str) {
        size_t len = strlen(str);
        ssize_t written = write(fd, str, len);
        (void)written;
    }
    close(fd);
    return path;
}

/* Mở write transaction nếu chưa có */
static int ensure_write_trans(maapi_session_t *m) {
    if (m->has_write) return 0;
    int th = maapi_start_trans(m->sock, CONFD_CANDIDATE, CONFD_READ_WRITE);
    if (th < 0) {
        fprintf(stderr, "[maapi] start_trans(write) failed: %s\n",
                confd_lasterr());
        return -1;
    }
    m->th_write  = th;
    m->has_write = true;
    return 0;
}

/* Đóng write transaction (không commit — dùng sau discard) */
static void drop_write_trans(maapi_session_t *m) {
    if (!m->has_write) return;
    maapi_finish_trans(m->sock, m->th_write);
    m->has_write = false;
}

/* ─── Lifecycle ─────────────────────────────────────────── */

maapi_session_t *maapi_dial(const char *host, int port, const char *user) {
    confd_init("cli-netconf-maapi", stderr, CONFD_SILENT);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_aton(host, &addr.sin_addr) == 0) {
        fprintf(stderr, "[maapi] invalid address: %s\n", host);
        return NULL;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    if (maapi_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != CONFD_OK) {
        fprintf(stderr, "[maapi] connect to %s:%d failed: %s\n",
                host, port, confd_lasterr());
        close(sock);
        return NULL;
    }

    if (maapi_start_user_session(sock,
                                 user ? user : "admin",
                                 "system",
                                 NULL, 0,
                                 CONFD_PROTO_EXTERNAL) != CONFD_OK) {
        fprintf(stderr, "[maapi] start_user_session failed: %s\n",
                confd_lasterr());
        maapi_close(sock);
        return NULL;
    }

    maapi_session_t *m = calloc(1, sizeof(*m));
    if (!m) { close(sock); return NULL; }
    m->sock = sock;
    m->port = port;
    strncpy(m->host, host, sizeof(m->host) - 1);
    return m;
}

void cli_session_close(maapi_session_t *m) {
    if (!m) return;
    if (m->has_write) {
        maapi_finish_trans(m->sock, m->th_write);
    }
    maapi_end_user_session(m->sock);
    maapi_close(m->sock);   /* ConfD's own maapi_close(int sock) */
    free(m);
}

/* ─── Read ──────────────────────────────────────────────── */

char *maapi_get_config_xml(maapi_session_t *m, int db) {
    int th = maapi_start_trans(m->sock, db, CONFD_READ);
    if (th < 0) {
        fprintf(stderr, "[maapi] start_trans(read) failed: %s\n",
                confd_lasterr());
        return NULL;
    }

    /* Ghi ra file tạm để đọc lại */
    char tmppath[] = "/tmp/maapi-cfg-XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) {
        maapi_finish_trans(m->sock, th);
        return NULL;
    }

    int rc = maapi_save_config(m->sock, th,
                               MAAPI_CONFIG_XML | MAAPI_CONFIG_WITH_DEFAULTS,
                               fd);
    close(fd);
    maapi_finish_trans(m->sock, th);

    if (rc != CONFD_OK) {
        fprintf(stderr, "[maapi] save_config failed: %s\n", confd_lasterr());
        unlink(tmppath);
        return NULL;
    }

    /* Đọc XML raw */
    char *raw = read_file_str(tmppath);
    unlink(tmppath);
    if (!raw) return NULL;

    /* Wrap trong envelope giống NETCONF get-config reply
     * → tương thích với fmt_xml_to_text() */
    size_t raw_len = strlen(raw);
    const char *pre  = "<rpc-reply xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
                       "<data>";
    const char *post = "</data></rpc-reply>";
    size_t total = strlen(pre) + raw_len + strlen(post) + 1;
    char *result = malloc(total);
    if (!result) { free(raw); return NULL; }
    snprintf(result, total, "%s%s%s", pre, raw, post);
    free(raw);
    return result;
}

/* ─── Write ─────────────────────────────────────────────── */

int maapi_set_value_str(maapi_session_t *m,
                        const char *keypath, const char *value) {
    if (ensure_write_trans(m) != 0) return -1;

    /* maapi_set_elem2 takes a plain string — no confd_value_t needed */
    if (maapi_set_elem2(m->sock, m->th_write, value, "%s", keypath) != CONFD_OK) {
        fprintf(stderr, "[maapi] set_elem2 %s = %s failed: %s\n",
                keypath, value, confd_lasterr());
        return -1;
    }
    return 0;
}

int maapi_load_xml(maapi_session_t *m, const char *xml) {
    if (!xml || !*xml) return -1;
    if (ensure_write_trans(m) != 0) return -1;

    /* Ghi XML vào file tạm */
    char *tmppath = write_tmp_str(xml);
    if (!tmppath) return -1;

    int rc = maapi_load_config(m->sock, m->th_write,
                               MAAPI_CONFIG_XML | MAAPI_CONFIG_MERGE,
                               tmppath);
    unlink(tmppath);
    free(tmppath);

    if (rc != CONFD_OK) {
        fprintf(stderr, "[maapi] load_config failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

int maapi_delete_node(maapi_session_t *m, const char *keypath) {
    if (ensure_write_trans(m) != 0) return -1;

    if (maapi_delete(m->sock, m->th_write, "%s", keypath) != CONFD_OK) {
        fprintf(stderr, "[maapi] delete %s failed: %s\n",
                keypath, confd_lasterr());
        return -1;
    }
    return 0;
}

/* ─── Transaction control ───────────────────────────────── */

int maapi_do_validate(maapi_session_t *m) {
    if (!m->has_write) return 0; /* nothing to validate */
    if (maapi_validate_trans(m->sock, m->th_write,
                             0 /* unlock */, 1 /* forcevalidation */)
        != CONFD_OK) {
        fprintf(stderr, "[maapi] validate failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

int maapi_do_commit(maapi_session_t *m) {
    /* apply_trans ghi write transaction vào candidate */
    if (m->has_write) {
        if (maapi_apply_trans(m->sock, m->th_write, 0) != CONFD_OK) {
            fprintf(stderr, "[maapi] apply_trans failed: %s\n", confd_lasterr());
            return -1;
        }
        maapi_finish_trans(m->sock, m->th_write);
        m->has_write = false;
    }

    /* commit candidate → running */
    if (maapi_candidate_commit(m->sock) != CONFD_OK) {
        fprintf(stderr, "[maapi] candidate_commit failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

int maapi_do_discard(maapi_session_t *m) {
    drop_write_trans(m);
    /* Reset candidate về running */
    if (maapi_candidate_reset(m->sock) != CONFD_OK) {
        fprintf(stderr, "[maapi] candidate_reset failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

int maapi_do_lock(maapi_session_t *m, int db) {
    if (maapi_lock(m->sock, db) != CONFD_OK) {
        fprintf(stderr, "[maapi] lock failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

int maapi_do_unlock(maapi_session_t *m, int db) {
    if (maapi_unlock(m->sock, db) != CONFD_OK) {
        fprintf(stderr, "[maapi] unlock failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

/* ─── Schema ─────────────────────────────────────────────── */

bool maapi_load_schema_into(maapi_session_t *m, schema_node_t **out_schema) {
    if (!m || !out_schema) return false;
    return maapi_load_schema(m->host, m->port, out_schema);
}

/* ─── Path conversion ───────────────────────────────────── */

char *args_to_keypath(schema_node_t *schema,
                      char **args, int argc,
                      int *consumed) {
    if (consumed) *consumed = 0;
    if (!schema || !args || argc == 0) return NULL;

    /* Tạo buffer đủ lớn */
    size_t cap = 512;
    char  *kp  = malloc(cap);
    if (!kp) return NULL;
    kp[0] = '\0';
    size_t len = 0;

    schema_node_t *node = schema; /* bắt đầu từ root */
    int i = 0;

    while (i < argc) {
        const char *token = args[i];

        /* Tìm child có tên khớp (case-insensitive) */
        schema_node_t *child = NULL;
        for (schema_node_t *c = node->children; c; c = c->next) {
            if (strcasecmp(c->name, token) == 0) { child = c; break; }
        }

        if (!child) {
            /* Không tìm thấy trong schema — vẫn append thẳng vào path
             * (cho phép dùng với node chưa có trong schema) */
            size_t needed = len + 1 + strlen(token) + 1;
            while (needed >= cap) { cap *= 2; kp = realloc(kp, cap); }
            len += (size_t)snprintf(kp + len, cap - len, "/%s", token);
            i++;
            if (consumed) *consumed = i;
            break; /* không biết is_list hay không → dừng */
        }

        /* Append tên node */
        size_t needed = len + 1 + strlen(child->name) + 1;
        while (needed >= cap) { cap *= 2; kp = realloc(kp, cap); }
        len += (size_t)snprintf(kp + len, cap - len, "/%s", child->name);
        i++;

        /* Nếu là list: token tiếp theo là key → bọc {} */
        if (child->is_list && i < argc) {
            const char *key = args[i];
            size_t kneeded = len + 1 + strlen(key) + 2;
            while (kneeded >= cap) { cap *= 2; kp = realloc(kp, cap); }
            len += (size_t)snprintf(kp + len, cap - len, "{%s}", key);
            i++;
        }

        if (consumed) *consumed = i;

        /* Dừng nếu là leaf — phần còn lại là value */
        if (child->is_leaf) break;

        node = child;
    }

    return kp; /* caller free */
}

#endif /* WITH_MAAPI */
