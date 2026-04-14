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
#define MAAPI_CONFIG_XML           1
#define MAAPI_CONFIG_WITH_DEFAULTS 2
#define MAAPI_CONFIG_MERGE         8

/* ─── cs_node flags ──────────────────────────────────────── */
#define CS_NODE_IS_LIST (1 << 0)

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
        uint8_t             _raw[24]; /* padding to match .so ABI */
    } val;
};
typedef struct confd_value confd_value_t;

#define CONFD_SET_STR(V, S)                                     \
    do {                                                         \
        (V)->type         = C_BUF;                              \
        (V)->val.buf.ptr  = (unsigned char *)(uintptr_t)(S);    \
        (V)->val.buf.size = (int)strlen(S);                     \
    } while (0)

/* ─── Namespace info ─────────────────────────────────────── */
struct confd_nsinfo {
    uint32_t    hash;
    const char *prefix;
    const char *uri;
    const char *revision;
};

/* ─── Schema tree (cs_node) ──────────────────────────────── */
/*
 * Minimal cs_node_info — only the fields we read.
 * Layout must match libconfd.so; adjust if schema walk crashes.
 */
struct confd_cs_node_info {
    uint32_t type;      /* enum confd_vtype */
    uint32_t flags;     /* CS_NODE_IS_LIST, etc. */
    uint32_t minOccurs;
    uint32_t maxOccurs;
    void    *keys;      /* key list pointer (unused) */
    void    *choices;
};

struct confd_cs_node {
    uint32_t               tag;
    uint32_t               ns;
    struct confd_cs_node_info info;
    struct confd_cs_node  *parent;
    struct confd_cs_node  *children;
    struct confd_cs_node  *next;
};

/* ─── confd_lib functions ────────────────────────────────── */
void        confd_init(const char *name, FILE *estream, int debug_level);
const char *confd_lasterr(void);
const char *confd_hash2str(uint32_t hash);
int         confd_get_nslist(struct confd_nsinfo **listp);
struct confd_cs_node *confd_find_cs_root(uint32_t ns);

/* ─── MAAPI functions ────────────────────────────────────── */

/* Connect to ConfD IPC socket */
int maapi_connect(int sock, struct sockaddr *srv, int srv_sz);

/*
 * Start user session.
 * proto: CONFD_PROTO_TCP (2) or CONFD_PROTO_EXTERNAL (8)
 */
int maapi_start_user_session(int sock,
                              const char  *username,
                              const char  *context,
                              const char **groups,
                              int          num_groups,
                              int          proto);
int maapi_end_user_session(int sock);

/*
 * Start transaction — returns th handle (>= 0) on success, CONFD_ERR on fail.
 * rw: CONFD_READ or CONFD_READ_WRITE
 */
int maapi_start_trans(int sock, int dbname, int rw);

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
 * Save config from a transaction to a file descriptor (XML).
 * flags: MAAPI_CONFIG_XML | MAAPI_CONFIG_WITH_DEFAULTS
 */
int maapi_save_config(int sock, int thandle, int flags, int fd);

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
