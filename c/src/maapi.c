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

#include <confd_lib.h>
#include <confd_maapi.h>

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

        /* Xác định type */
        switch (node->info.type) {
            case C_XMLTAG:  /* container hoặc list */
                child->is_list = (node->info.flags & CS_NODE_IS_LIST) != 0;
                child->is_leaf = false;
                break;
            case C_BUF:
            case C_INT8:  case C_INT16:  case C_INT32:  case C_INT64:
            case C_UINT8: case C_UINT16: case C_UINT32: case C_UINT64:
            case C_BOOL:
            case C_ENUM_VALUE:
            case C_IPV4:  case C_IPV6:
            case C_DECIMAL64:
                child->is_leaf = true;
                break;
            default:
                child->is_leaf = (node->children == NULL);
                break;
        }

        /* Recurse */
        if (!child->is_leaf) {
            walk_cs_node(node, child);
        }
    }
}

/* -------------------------------------------------------------------------
 * maapi_load_schema — kết nối MAAPI, walk schema tree, fill s->schema
 * Trả về true nếu thành công
 * ---------------------------------------------------------------------- */
bool maapi_load_schema(cli_session_t *s) {
    /* Kết nối tới ConfD */
    const char *host = getenv("CONFD_IPC_ADDR");
    if (!host) host = MAAPI_DEFAULT_HOST;

    int port = MAAPI_DEFAULT_PORT;
    const char *port_env = getenv("CONFD_IPC_PORT");
    if (port_env) port = atoi(port_env);

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
        close(sock);
        return false;
    }

    if (maapi_start_user_session(sock, "admin", "system", NULL, 0,
                                 CONFD_PROTO_TCP) != CONFD_OK) {
        close(sock);
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

    /* Với mỗi namespace, lấy cs_node root và walk */
    for (int i = 0; i < ns_count; i++) {
        struct confd_cs_node *root_cs =
            confd_find_cs_root(ns_list[i].hash);
        if (!root_cs) continue;

        /*
         * Top-level node từ confd_find_cs_root là schema root của namespace.
         * Ta tạo schema_node cho mỗi top-level container và walk sâu xuống.
         */
        for (struct confd_cs_node *top = root_cs; top; top = top->next) {
            const char *top_name = confd_hash2str(top->tag);
            if (!top_name) continue;

            /* Tìm hoặc tạo top-level node trong s->schema */
            schema_node_t *sn = NULL;
            for (schema_node_t *c = s->schema->children; c; c = c->next) {
                if (strcmp(c->name, top_name) == 0) { sn = c; break; }
            }
            if (!sn) {
                sn = schema_new_node(top_name);
                if (!sn) continue;
                /* Namespace URI */
                if (ns_list[i].uri)
                    strncpy(sn->ns, ns_list[i].uri, MAX_NS_LEN - 1);
                sn->next = s->schema->children;
                s->schema->children = sn;
            }

            /* Walk children của top-level node */
            walk_cs_node(top, sn);
        }
    }

    free(ns_list);
    maapi_end_user_session(sock);
    close(sock);

    fprintf(stderr, "[maapi] schema loaded from ConfD (%s:%d)\n", host, port);
    return true;
}

#endif /* WITH_MAAPI */
