/*
 * confd_compat.h — Minimal ConfD API definitions bundled in project.
 *
 * Replaces confd_lib.h + confd_maapi.h from the ConfD SDK.
 * Only declares what cli-netconf actually uses.
 * Link against: libconfd.so
 */
#ifndef CONFD_COMPAT_H
#define CONFD_COMPAT_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Return codes ───────────────────────────────────────── */
#define CONFD_OK    0
#define CONFD_ERR  -1
#define CONFD_EOF  -2

/* ─── Debug levels ───────────────────────────────────────── */
#define CONFD_SILENT 0
#define CONFD_DEBUG  1
#define CONFD_TRACE  2

/* ─── IPC port ───────────────────────────────────────────── */
#ifndef CONFD_PORT
#define CONFD_PORT 4565
#endif

/* ─── Datastores ─────────────────────────────────────────── */
#define CONFD_RUNNING   1
#define CONFD_CANDIDATE 2
#define CONFD_STARTUP   3

/* ─── Transaction access mode ────────────────────────────── */
#define CONFD_READ        1
#define CONFD_READ_WRITE  2

/* ─── Protocol types (for maapi_start_user_session) ─────── */
#define CONFD_PROTO_TCP      2
#define CONFD_PROTO_EXTERNAL 8

/* ─── MAAPI save/load config flags ───────────────────────── */
#define MAAPI_CONFIG_XML            (1 << 0)
#define MAAPI_CONFIG_WITH_DEFAULTS  (1 << 3)
#define MAAPI_CONFIG_SHOW_DEFAULTS  (1 << 4)
#define MAAPI_CONFIG_MERGE          (1 << 5)

/* ─── cs_node flags ──────────────────────────────────────── */
#define CS_NODE_IS_LIST      (1 << 0)
#define CS_NODE_IS_CONTAINER (1 << 8)

/* ─── Value types ────────────────────────────────────────── */
enum confd_vtype {
    C_NOEXISTS    = 0,
    C_XMLTAG      = 1,
    C_BUF         = 5,
    C_INT8        = 6,
    C_INT16       = 7,
    C_INT32       = 8,
    C_INT64       = 9,
    C_UINT8       = 10,
    C_UINT16      = 11,
    C_UINT32      = 12,
    C_UINT64      = 13,
    C_DOUBLE      = 14,
    C_IPV4        = 15,
    C_IPV6        = 16,
    C_BOOL        = 17,
    C_QNAME       = 18,
    C_DATETIME    = 19,
    C_DATE        = 20,
    C_TIME        = 21,
    C_DURATION    = 22,
    C_ENUM_VALUE  = 23,
    C_LIST        = 24,
    C_OBJECTREF   = 25,
    C_OID         = 26,
    C_BINARY      = 27,
    C_DECIMAL64   = 28,
    C_IDENTITYREF = 29,
    C_UNION       = 30,
    C_INSTANCE_IDENTIFIER = 31,
};

/* ─── confd_value_t ──────────────────────────────────────── */
struct confd_buf_t {
    unsigned char *ptr;
    int            size;
};

struct confd_value {
    uint8_t type;       /* enum confd_vtype */
    uint8_t _pad[3];
    union {
        int8_t              i8;
        uint8_t             u8;
        int16_t             i16;
        uint16_t            u16;
        int32_t             i32;
        uint32_t            u32;
        int64_t             i64;
        uint64_t            u64;
        double              dbl;
        struct confd_buf_t  buf;
        uint8_t             _raw[32]; /* padding to match .so ABI (sizeof=40) */
    } val;
};
typedef struct confd_value confd_value_t;

#define CONFD_SET_STR(V, S)                                     \
    do {                                                         \
        (V)->type         = C_BUF;                              \
        (V)->val.buf.ptr  = (unsigned char *)(uintptr_t)(S);    \
        (V)->val.buf.size = (int)strlen(S);                     \
    } while (0)

/* ─── Namespace info (must match confd_lib.h layout) ─────── */
struct confd_nsinfo {
    const char *uri;
    const char *prefix;
    uint32_t    hash;
    const char *revision;
    const char *module;
};

/* ─── Schema tree (cs_node) ──────────────────────────────── */
/*
 * cs_node_info — must match libconfd.so layout exactly.
 * Based on confd_lib.h from ConfD 7.3 SDK.
 */
struct confd_type; /* forward decl (opaque) */
struct confd_cs_choice; /* forward decl (opaque) */
struct confd_cs_meta_data; /* forward decl (opaque) */

struct confd_cs_node_info {
    uint32_t      *keys;
    int            minOccurs;
    int            maxOccurs;
    uint32_t       shallow_type;   /* enum confd_vtype */
    struct confd_type *type;
    confd_value_t *defval;
    struct confd_cs_choice *choices;
    int            flags;
    uint8_t        cmp;
    struct confd_cs_meta_data *meta_data;
};

struct confd_cs_node {
    uint32_t               tag;
    uint32_t               ns;
    struct confd_cs_node_info info;
    struct confd_cs_node  *parent;
    struct confd_cs_node  *children;
    struct confd_cs_node  *next;
    void                  *opaque;
};

/* ─── confd_lib functions ────────────────────────────────── */

/*
 * confd_init — the .so exports confd_init_vsn(name, estream, debug, sz, ver)
 * We wrap it as a macro so existing code calling confd_init(name,stream,level) works.
 */
/*
 * confd_init_vsn_sz(name, estream, debug, api_vsn, maxdepth, maxkeylen)
 * — the real signature from confd_lib.h.
 * CONFD_LIB_API_VSN = 0x07030000 (ConfD 7.3)
 * MAXDEPTH = 20, MAXKEYLEN = 9 (default ConfD constants)
 */
#define CONFD_LIB_API_VSN 0x07030000
#ifndef MAXDEPTH
#define MAXDEPTH  20
#endif
#ifndef MAXKEYLEN
#define MAXKEYLEN 9
#endif

void confd_init_vsn_sz(const char *name, FILE *estream, int debug_level,
                       int api_vsn, int maxdepth, int maxkeylen);
#define confd_init(name, estream, debug) \
    confd_init_vsn_sz((name), (estream), (debug), \
                      CONFD_LIB_API_VSN, MAXDEPTH, MAXKEYLEN)

int         confd_load_schemas(const struct sockaddr *srv, int srv_sz);
const char *confd_lasterr(void);
const char *confd_hash2str(uint32_t hash);
int         confd_get_nslist(struct confd_nsinfo **listp);
struct confd_cs_node *confd_find_cs_root(uint32_t ns);

/* ─── confd_ip (needed for maapi_start_user_session) ─────── */
struct confd_ip {
    int af;    /* AF_INET | AF_INET6 */
    union {
        struct in_addr  v4;
        struct in6_addr v6;
    } ip;
};

/* ─── MAAPI functions ────────────────────────────────────── */

/* Connect to ConfD IPC socket */
int maapi_connect(int sock, struct sockaddr *srv, int srv_sz);

/*
 * maapi_start_user_session3 — 12 params in ConfD 7.3:
 *   (sock, user, ctx, groups, ngrps, src_addr, src_port, proto,
 *    vendor, product, version, client_id)
 */
int maapi_start_user_session3(int sock,
                              const char  *username,
                              const char  *context,
                              const char **groups,
                              int          num_groups,
                              const struct confd_ip *src_addr,
                              int          src_port,
                              int          proto,
                              const char  *vendor,
                              const char  *product,
                              const char  *version,
                              const char  *client_id);

/* Wrapper: src_addr=NULL may segfault in some ConfD versions,
 * so we pass a zeroed-out localhost address */
static inline int _maapi_start_session_compat(
    int sock, const char *user, const char *ctx,
    const char **grps, int ngrps, int proto) {
    struct confd_ip src = {0};
    src.af = AF_INET; /* 0.0.0.0 */
    return maapi_start_user_session3(sock, user, ctx, grps, ngrps,
                                     &src, 0, proto,
                                     NULL, NULL, NULL, NULL);
}
#define maapi_start_user_session(sock, user, ctx, grps, ngrps, proto) \
    _maapi_start_session_compat((sock), (user), (ctx), (grps), (ngrps), (proto))
int maapi_end_user_session(int sock);

/*
 * maapi_start_trans_flags2 — 9 params in ConfD 7.3:
 *   (sock, db, rw, usid, flags, vendor, product, version, client_id)
 */
int maapi_start_trans_flags2(int sock, int dbname, int rw,
                             int usid, int flags,
                             const char *vendor,
                             const char *product,
                             const char *version,
                             const char *client_id);
#define maapi_start_trans(sock, dbname, rw) \
    maapi_start_trans_flags2((sock), (dbname), (rw), 0, 0, \
                             NULL, NULL, NULL, NULL)

/* Close/finish a transaction (no commit) */
int maapi_finish_trans(int sock, int thandle);

/* Apply (write transaction → datastore) then commit candidate → running */
int maapi_apply_trans(int sock, int thandle, int keepopen);
int maapi_validate_trans(int sock, int thandle, int unlock, int forcevalidation);
int maapi_candidate_commit(int sock);
int maapi_candidate_reset(int sock);

/* Lock/unlock a datastore */
int maapi_lock(int sock, int dbname);
int maapi_unlock(int sock, int dbname);

/*
 * Save config from a transaction to a file (XML).
 * flags: MAAPI_CONFIG_XML | MAAPI_CONFIG_WITH_DEFAULTS
 * fmtpath: printf-style path format, e.g. "%s" with file path arg.
 */
int maapi_save_config(int sock, int thandle, int flags,
                      const char *fmtpath, ...);
int maapi_save_config_result(int sock, int id);
int confd_stream_connect(int sock, const struct sockaddr *srv,
                         int srv_sz, int id, int flags);

/*
 * Load config from a file into a write transaction.
 * flags: MAAPI_CONFIG_XML | MAAPI_CONFIG_MERGE
 */
int maapi_load_config(int sock, int thandle, int flags, const char *path);

/*
 * Set a leaf value.
 * fmt is a keypath format string, e.g. "/system/hostname"
 */
int maapi_set_elem(int sock, int thandle, confd_value_t *v,
                   const char *fmt, ...);

/*
 * Set a leaf value from a plain string (no confd_value_t needed).
 * Preferred over maapi_set_elem when available.
 */
int maapi_set_elem2(int sock, int thandle, const char *strval,
                    const char *fmt, ...);

/* Delete a node */
int maapi_delete(int sock, int thandle, const char *fmt, ...);

/*
 * ConfD's own maapi_close — closes the MAAPI socket.
 * NOTE: do NOT name your own function "maapi_close" to avoid conflicts.
 */
int maapi_close(int sock);

#ifdef __cplusplus
}
#endif

#endif /* CONFD_COMPAT_H */
