/*
 * netconf.c — NETCONF client over TCP (plain) hoặc SSH (libssh2)
 *
 * Hỗ trợ:
 *   nc_dial_tcp  — kết nối thẳng TCP (ConfD port 2023, mock :2023)
 *   nc_dial_ssh  — kết nối SSH (NE port 830, mock :8830)
 *   Tất cả NETCONF operations cần cho CLI
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdarg.h>

#ifdef WITH_SSH
#include <libssh2.h>
#endif

#include "cli.h"

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

static uint64_t next_msg_id(netconf_session_t *nc) {
    return ++nc->msg_id;
}

/* Đọc cho đến khi gặp delimiter ]]>]]> */
static char *nc_read_msg(netconf_session_t *nc) {
    size_t cap  = 65536;
    size_t len  = 0;
    char  *buf  = malloc(cap);
    if (!buf) return NULL;

    while (1) {
        /* Tăng buffer nếu cần */
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }

        char c;
        ssize_t n;

#ifdef WITH_SSH
        if (nc->ssh_channel) {
            n = libssh2_channel_read(nc->ssh_channel, &c, 1);
            if (n <= 0) { free(buf); return NULL; }
        } else
#endif
        {
            n = recv(nc->fd, &c, 1, 0);
            if (n <= 0) { free(buf); return NULL; }
        }

        buf[len++] = c;
        buf[len]   = '\0';

        /* Kiểm tra delimiter ]]>]]> */
        if (len >= NETCONF_DELIM_LEN &&
            memcmp(buf + len - NETCONF_DELIM_LEN,
                   NETCONF_DELIM, NETCONF_DELIM_LEN) == 0) {
            buf[len - NETCONF_DELIM_LEN] = '\0';
            /* Skip leading whitespace (trailing \n from previous msg) */
            char *start = buf;
            while (*start == '\n' || *start == '\r' || *start == ' ') start++;
            if (start != buf) memmove(buf, start, strlen(start) + 1);
            return buf;
        }
    }
}

/* Gửi message có thêm delimiter */
static bool nc_write_msg(netconf_session_t *nc, const char *msg) {
    size_t msg_len   = strlen(msg);
    size_t delim_len = NETCONF_DELIM_LEN;
    size_t total     = msg_len + delim_len;
    char  *frame     = malloc(total + 1);
    if (!frame) return false;

    memcpy(frame, msg, msg_len);
    memcpy(frame + msg_len, NETCONF_DELIM, delim_len);
    frame[total] = '\0';

    bool ok;
#ifdef WITH_SSH
    if (nc->ssh_channel) {
        ssize_t sent = libssh2_channel_write(nc->ssh_channel, frame, total);
        ok = (sent == (ssize_t)total);
    } else
#endif
    {
        ssize_t sent = send(nc->fd, frame, total, 0);
        ok = (sent == (ssize_t)total);
    }
    free(frame);
    return ok;
}

/* Parse session-id và capabilities từ server HELLO */
static void parse_hello(netconf_session_t *nc, const char *hello) {
    /* session-id */
    const char *sid_start = strstr(hello, "<session-id>");
    if (sid_start) {
        sid_start += strlen("<session-id>");
        const char *sid_end = strstr(sid_start, "</session-id>");
        if (sid_end) {
            size_t len = (size_t)(sid_end - sid_start);
            if (len >= sizeof(nc->session_id)) len = sizeof(nc->session_id) - 1;
            strncpy(nc->session_id, sid_start, len);
            nc->session_id[len] = '\0';
        }
    }

    /* capabilities — đếm số lượng <capability> */
    int count = 0;
    const char *p = hello;
    while ((p = strstr(p, "<capability>")) != NULL) { count++; p++; }

    if (count == 0) return;
    nc->capabilities = calloc(count + 1, sizeof(char *));
    if (!nc->capabilities) return;

    p = hello;
    int i = 0;
    while ((p = strstr(p, "<capability>")) != NULL && i < count) {
        p += strlen("<capability>");
        const char *end = strstr(p, "</capability>");
        if (!end) break;
        size_t len = (size_t)(end - p);
        nc->capabilities[i] = malloc(len + 1);
        if (nc->capabilities[i]) {
            memcpy(nc->capabilities[i], p, len);
            nc->capabilities[i][len] = '\0';
        }
        i++;
        p = end + 1;
    }
    nc->cap_count = i;
}

/* Gửi client HELLO và nhận server HELLO */
static bool do_hello_exchange(netconf_session_t *nc) {
    const char *client_hello =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">\n"
        "  <capabilities>\n"
        "    <capability>urn:ietf:params:netconf:base:1.0</capability>\n"
        "  </capabilities>\n"
        "</hello>";

    /* Nhận server HELLO trước */
    char *server_hello = nc_read_msg(nc);
    if (!server_hello) return false;
    parse_hello(nc, server_hello);
    free(server_hello);

    /* Gửi client HELLO */
    return nc_write_msg(nc, client_hello);
}

/* -------------------------------------------------------------------------
 * nc_dial_tcp — kết nối NETCONF qua TCP thuần
 * ---------------------------------------------------------------------- */
netconf_session_t *nc_dial_tcp(const char *host, int port) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        fprintf(stderr, "getaddrinfo(%s:%d): %s\n", host, port, strerror(errno));
        return NULL;
    }

    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        fprintf(stderr, "connect(%s:%d): %s\n", host, port, strerror(errno));
        return NULL;
    }

    netconf_session_t *nc = calloc(1, sizeof(*nc));
    if (!nc) { close(fd); return NULL; }
    nc->fd = fd;

    if (!do_hello_exchange(nc)) {
        nc_close(nc);
        return NULL;
    }
    return nc;
}

/* -------------------------------------------------------------------------
 * nc_dial_ssh — kết nối NETCONF qua SSH (libssh2)
 * ---------------------------------------------------------------------- */
netconf_session_t *nc_dial_ssh(const char *host, int port,
                               const char *user, const char *pass) {
#ifndef WITH_SSH
    (void)host; (void)port; (void)user; (void)pass;
    fprintf(stderr, "SSH mode: compile with -DWITH_SSH and link libssh2\n");
    return NULL;
#else
    /* Khởi tạo libssh2 (một lần) */
    static bool ssh2_inited = false;
    if (!ssh2_inited) {
        libssh2_init(0);
        ssh2_inited = true;
    }

    /* Tạo TCP socket */
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return NULL;

    int fd = -1;
    for (struct addrinfo *r = res; r; r = r->ai_next) {
        fd = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, r->ai_addr, r->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return NULL;

    /* SSH session */
    LIBSSH2_SESSION *ssh = libssh2_session_init();
    if (!ssh) { close(fd); return NULL; }
    libssh2_session_set_blocking(ssh, 1);

    if (libssh2_session_handshake(ssh, fd) != 0) {
        libssh2_session_free(ssh); close(fd); return NULL;
    }
    if (libssh2_userauth_password(ssh, user, pass) != 0) {
        libssh2_session_disconnect(ssh, "auth failed");
        libssh2_session_free(ssh); close(fd); return NULL;
    }

    /* Mở channel và request "netconf" subsystem */
    LIBSSH2_CHANNEL *ch = libssh2_channel_open_session(ssh);
    if (!ch) {
        libssh2_session_disconnect(ssh, "no channel");
        libssh2_session_free(ssh); close(fd); return NULL;
    }
    if (libssh2_channel_subsystem(ch, "netconf") != 0) {
        libssh2_channel_free(ch);
        libssh2_session_disconnect(ssh, "no netconf subsystem");
        libssh2_session_free(ssh); close(fd); return NULL;
    }

    netconf_session_t *nc = calloc(1, sizeof(*nc));
    if (!nc) { libssh2_channel_free(ch); libssh2_session_free(ssh); close(fd); return NULL; }
    nc->fd          = fd;
    nc->ssh_session = ssh;
    nc->ssh_channel = ch;

    if (!do_hello_exchange(nc)) { nc_close(nc); return NULL; }
    return nc;
#endif
}

/* -------------------------------------------------------------------------
 * nc_close
 * ---------------------------------------------------------------------- */
void nc_close(netconf_session_t *nc) {
    if (!nc) return;
#ifdef WITH_SSH
    if (nc->ssh_channel) {
        libssh2_channel_send_eof(nc->ssh_channel);
        libssh2_channel_free(nc->ssh_channel);
    }
    if (nc->ssh_session) {
        libssh2_session_disconnect(nc->ssh_session, "bye");
        libssh2_session_free(nc->ssh_session);
    }
#endif
    if (nc->fd >= 0) close(nc->fd);
    if (nc->capabilities) {
        for (int i = 0; i < nc->cap_count; i++) free(nc->capabilities[i]);
        free(nc->capabilities);
    }
    free(nc);
}

/* -------------------------------------------------------------------------
 * nc_send_rpc — gửi body RPC, trả về reply (caller free)
 * ---------------------------------------------------------------------- */
char *nc_send_rpc(netconf_session_t *nc, const char *body) {
    char *rpc = NULL;
    unsigned long long mid = (unsigned long long)next_msg_id(nc);
    int len = asprintf(&rpc,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\""
        " message-id=\"%llu\">%s</rpc>",
        mid, body);
    if (len < 0 || !rpc) return NULL;

    bool ok = nc_write_msg(nc, rpc);
    free(rpc);
    if (!ok) return NULL;

    return nc_read_msg(nc);
}

/* -------------------------------------------------------------------------
 * NETCONF operations
 * ---------------------------------------------------------------------- */
char *nc_get_config(netconf_session_t *nc, const char *datastore,
                    const char *filter) {
    char *body;
    if (filter && strlen(filter) > 0) {
        asprintf(&body,
            "<get-config>"
            "<source><%s/></source>"
            "<filter type=\"subtree\">%s</filter>"
            "</get-config>", datastore, filter);
    } else {
        asprintf(&body,
            "<get-config><source><%s/></source></get-config>",
            datastore);
    }
    char *reply = nc_send_rpc(nc, body);
    free(body);
    return reply;
}

char *nc_edit_config(netconf_session_t *nc, const char *datastore,
                     const char *xml) {
    char *body;
    asprintf(&body,
        "<edit-config>"
        "<target><%s/></target>"
        "<default-operation>merge</default-operation>"
        "<config>%s</config>"
        "</edit-config>", datastore, xml);
    char *reply = nc_send_rpc(nc, body);
    free(body);
    return reply;
}

char *nc_commit(netconf_session_t *nc) {
    return nc_send_rpc(nc, "<commit/>");
}

char *nc_validate(netconf_session_t *nc, const char *datastore) {
    char *body;
    asprintf(&body, "<validate><source><%s/></source></validate>", datastore);
    char *reply = nc_send_rpc(nc, body);
    free(body);
    return reply;
}

char *nc_discard(netconf_session_t *nc) {
    return nc_send_rpc(nc, "<discard-changes/>");
}

char *nc_lock(netconf_session_t *nc, const char *datastore) {
    char *body;
    asprintf(&body, "<lock><target><%s/></target></lock>", datastore);
    char *reply = nc_send_rpc(nc, body);
    free(body);
    return reply;
}

char *nc_unlock(netconf_session_t *nc, const char *datastore) {
    char *body;
    asprintf(&body, "<unlock><target><%s/></target></unlock>", datastore);
    char *reply = nc_send_rpc(nc, body);
    free(body);
    return reply;
}

char *nc_copy_config(netconf_session_t *nc, const char *target,
                     const char *xml) {
    char *body;
    asprintf(&body,
        "<copy-config>"
        "<target><%s/></target>"
        "<source><config>%s</config></source>"
        "</copy-config>", target, xml);
    char *reply = nc_send_rpc(nc, body);
    free(body);
    return reply;
}

char *nc_get_schema(netconf_session_t *nc, const char *identifier) {
    char *body;
    asprintf(&body,
        "<get-schema"
        " xmlns=\"urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring\">"
        "<identifier>%s</identifier>"
        "<format>yang</format>"
        "</get-schema>", identifier);
    char *reply = nc_send_rpc(nc, body);
    free(body);
    return reply;
}

/* -------------------------------------------------------------------------
 * nc_extract_modules — lấy danh sách module từ capabilities
 * Caller free: free mỗi string, rồi free mảng
 * ---------------------------------------------------------------------- */
char **nc_extract_modules(netconf_session_t *nc, int *count) {
    *count = 0;
    if (!nc->capabilities) return NULL;

    /* Đếm trước */
    int n = 0;
    for (int i = 0; i < nc->cap_count; i++) {
        if (strstr(nc->capabilities[i], "?module=")) n++;
    }
    if (n == 0) return NULL;

    char **modules = calloc(n + 1, sizeof(char *));
    if (!modules) return NULL;

    int idx = 0;
    for (int i = 0; i < nc->cap_count && idx < n; i++) {
        const char *p = strstr(nc->capabilities[i], "?module=");
        if (!p) continue;
        p += 8; /* bỏ "?module=" */
        const char *end = strpbrk(p, "&;");
        size_t len = end ? (size_t)(end - p) : strlen(p);
        modules[idx] = malloc(len + 1);
        if (modules[idx]) {
            memcpy(modules[idx], p, len);
            modules[idx][len] = '\0';
            idx++;
        }
    }
    *count = idx;
    return modules;
}
