/*
 * maapi.c — MAAPI schema provider (libconfd, optional)
 *
 * Compile: make WITH_MAAPI=1 CONFD_DIR=/path/to/confd
 *
 * MAAPI (Management Agent API) là IPC protocol nội bộ của ConfD.
 * Cho phép walk schema tree trực tiếp — không cần YANG files, không bị
 * ảnh hưởng bởi ConfD omitting containers với default values.
 *
 * Khi WITH_MAAPI được bật, schema_load() trong schema.c sẽ gọi
 * maapi_load_schema() trước các phương thức NETCONF/XML.
 *
 * Yêu cầu:
 *   - ConfD đang chạy trên cùng host (hoặc CONFD_IPC_ADDR accessible)
 *   - libconfd.a / libconfd.so từ $CONFD_DIR/lib/
 *   - Headers từ $CONFD_DIR/include/
 */
#ifdef WITH_MAAPI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "confd_compat.h"

#include "cli.h"

/* Địa chỉ MAAPI (có thể override qua env CONFD_IPC_ADDR / CONFD_IPC_PORT) */
#define MAAPI_DEFAULT_HOST "127.0.0.1"
#define MAAPI_DEFAULT_PORT CONFD_PORT   /* thường là 4565 */

/* -------------------------------------------------------------------------
 * Recursive walk cs_node tree → build schema_node tree
 * ---------------------------------------------------------------------- */
static void walk_cs_node(struct confd_cs_node *cs,
                          schema_node_t       *parent) {
    if (!cs) return;

    for (struct confd_cs_node *node = cs->children; node; node = node->next) {
        const char *name = confd_hash2str(node->tag);
        if (!name) continue;

        schema_node_t *child = NULL;
        /* Tìm hoặc tạo child */
        for (schema_node_t *c = parent->children; c; c = c->next) {
            if (strcmp(c->name, name) == 0) { child = c; break; }
        }
        if (!child) {
            child = schema_new_node(name);
            if (!child) continue;
            child->next      = parent->children;
            parent->children = child;
        }

        /* Determine if leaf or container/list.
         * Use children pointer as heuristic — avoids depending on
         * cs_node_info struct layout which varies across ConfD versions. */
        child->is_leaf = (node->children == NULL);
        child->is_list = false; /* list detection requires info.flags */

        /* Recurse */
        if (!child->is_leaf) {
            walk_cs_node(node, child);
        }
    }
}

/* -------------------------------------------------------------------------
 * maapi_load_schema — connect to MAAPI, walk schema tree, fill *out_schema.
 * host/port: ConfD IPC address. Returns true on success.
 * ---------------------------------------------------------------------- */
bool maapi_load_schema(const char *host_arg, int port_arg,
                       schema_node_t **out_schema) {
    if (!out_schema) return false;

    const char *host = host_arg ? host_arg : MAAPI_DEFAULT_HOST;
    int port = (port_arg > 0) ? port_arg : MAAPI_DEFAULT_PORT;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    inet_aton(host, &addr.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    /* Khởi tạo libconfd */
    confd_init("cli-netconf-c", stderr, CONFD_SILENT);

    if (maapi_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != CONFD_OK) {
        maapi_close(sock);
        return false;
    }

    if (maapi_start_user_session(sock, "admin", "system", NULL, 0,
                                 CONFD_PROTO_EXTERNAL) != CONFD_OK) {
        maapi_close(sock);
        return false;
    }

    /* Load tất cả namespaces đã đăng ký với ConfD */
    struct confd_nsinfo *ns_list = NULL;
    int ns_count = confd_get_nslist(&ns_list);
    if (ns_count <= 0) {
        maapi_end_user_session(sock);
        close(sock);
        return false;
    }

    /* Allocate root node if not provided */
    if (!*out_schema) {
        *out_schema = schema_new_node("__root__");
        if (!*out_schema) {
            maapi_end_user_session(sock);
            close(sock);
            free(ns_list);
            return false;
        }
    }
    schema_node_t *root = *out_schema;

    /* Walk each namespace's cs_node tree */
    for (int i = 0; i < ns_count; i++) {
        struct confd_cs_node *root_cs =
            confd_find_cs_root(ns_list[i].hash);
        if (!root_cs) continue;

        for (struct confd_cs_node *top = root_cs; top; top = top->next) {
            const char *top_name = confd_hash2str(top->tag);
            if (!top_name) continue;

            /* Find or create top-level schema node */
            schema_node_t *sn = NULL;
            for (schema_node_t *c = root->children; c; c = c->next) {
                if (strcmp(c->name, top_name) == 0) { sn = c; break; }
            }
            if (!sn) {
                sn = schema_new_node(top_name);
                if (!sn) continue;
                if (ns_list[i].uri)
                    strncpy(sn->ns, ns_list[i].uri, MAX_NS_LEN - 1);
                sn->next = root->children;
                root->children = sn;
            }

            walk_cs_node(top, sn);
        }
    }

    free(ns_list);
    maapi_end_user_session(sock);
    maapi_close(sock);

    fprintf(stderr, "[maapi] schema loaded from ConfD (%s:%d)\n", host, port);
    return true;
}

#endif /* WITH_MAAPI */
