/*
 * maapi.c — Cung cấp schema thông qua MAAPI (libconfd, tuỳ chọn)
 *
 * Biên dịch: make WITH_MAAPI=1 CONFD_DIR=/path/to/confd
 *
 * MAAPI (Management Agent API) là giao thức IPC nội bộ của ConfD.
 * Cho phép duyệt (walk) schema tree trực tiếp — không cần YANG files,
 * không bị ảnh hưởng bởi việc ConfD bỏ qua container có giá trị mặc định.
 *
 * Khi WITH_MAAPI được bật, schema_load() trong schema.c sẽ gọi
 * maapi_load_schema() trước các phương thức NETCONF/XML.
 *
 * Yêu cầu:
 *   - ConfD đang chạy trên cùng host (hoặc CONFD_IPC_ADDR truy cập được)
 *   - libconfd.a / libconfd.so từ $CONFD_DIR/lib/
 *   - Headers từ $CONFD_DIR/include/
 */
#ifdef WITH_MAAPI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "confd_compat.h"

#include "cli.h"

/* Địa chỉ MAAPI mặc định (có thể ghi đè qua biến môi trường
 * CONFD_IPC_ADDR và CONFD_IPC_PORT) */
#define MAAPI_DEFAULT_HOST "127.0.0.1"
#define MAAPI_DEFAULT_PORT CONFD_PORT   /* Thường là 4565 */

/* -------------------------------------------------------------------------
 * walk_cs_node — Duyệt đệ quy cây cs_node của ConfD và xây dựng schema_node
 *
 * ConfD lưu trữ schema trong cấu trúc confd_cs_node (compiled schema node).
 * Hàm này chuyển đổi cây cs_node thành cây schema_node_t riêng của CLI,
 * chỉ giữ lại thông tin cần thiết cho tab completion (tên, loại node).
 *
 * Tham số:
 *   cs     — node cs_node gốc cần duyệt (ConfD internal)
 *   parent — node schema_node cha để gắn các node con vào
 * ---------------------------------------------------------------------- */
static void walk_cs_node(struct confd_cs_node *cs,
                          schema_node_t       *parent) {
    if (!cs) return;

    /* Duyệt qua tất cả node con của cs_node */
    for (struct confd_cs_node *node = cs->children; node; node = node->next) {
        /* Chuyển hash tag thành tên chuỗi — bỏ qua nếu không có tên */
        const char *name = confd_hash2str(node->tag);
        if (!name) continue;

        /* Tìm hoặc tạo node con tương ứng trong schema tree */
        schema_node_t *child = NULL;
        for (schema_node_t *c = parent->children; c; c = c->next) {
            if (strcmp(c->name, name) == 0) { child = c; break; }
        }
        if (!child) {
            child = schema_new_node(name);
            if (!child) continue;
            /* Chèn vào đầu danh sách liên kết children */
            child->next      = parent->children;
            parent->children = child;
        }

        /* Xác định loại node từ cờ (flags) trong cs_node_info:
         *   - CS_NODE_IS_LIST: node là YANG list (có key)
         *   - Không có children và không phải list/container: là leaf */
        child->is_list = (node->info.flags & CS_NODE_IS_LIST) != 0;
        child->is_leaf = (node->children == NULL) &&
                         !(node->info.flags & (CS_NODE_IS_LIST | CS_NODE_IS_CONTAINER));

        /* Với list, lấy danh sách tên key từ node->info.keys (mảng uint32_t
         * hash tags, kết thúc bằng 0). Dùng cho tab completion (ẩn key leaf)
         * và validate trong cmd_set. */
        if (child->is_list && node->info.keys) {
            child->n_keys = 0;
            for (int k = 0; node->info.keys[k] != 0 && k < MAX_LIST_KEYS; k++) {
                const char *kname = confd_hash2str(node->info.keys[k]);
                if (!kname) continue;
                strncpy(child->keys[child->n_keys], kname, MAX_NAME_LEN - 1);
                child->keys[child->n_keys][MAX_NAME_LEN - 1] = '\0';
                child->n_keys++;
            }
        }

        /* Đệ quy vào các node con (chỉ khi không phải leaf) */
        if (!child->is_leaf) {
            walk_cs_node(node, child);
        }
    }
}

/* -------------------------------------------------------------------------
 * maapi_load_schema — Kết nối tới ConfD qua MAAPI, duyệt schema tree,
 *                     và điền kết quả vào *out_schema.
 *
 * Quy trình:
 *   1. Kết nối TCP tới ConfD IPC port
 *   2. Mở user session
 *   3. Gọi confd_load_schemas() để tải tất cả namespace đã đăng ký
 *   4. Duyệt từng namespace, bỏ qua các namespace hệ thống (NETCONF, monitoring...)
 *   5. Với mỗi namespace cấu hình, duyệt cs_node tree và xây schema_node tree
 *
 * Tham số:
 *   host_arg   — địa chỉ ConfD IPC (NULL → dùng "127.0.0.1")
 *   port_arg   — cổng ConfD IPC (<=0 → dùng CONFD_PORT mặc định)
 *   out_schema — con trỏ output, trỏ tới schema tree đã xây dựng
 *
 * Trả về:
 *   true nếu tải schema thành công, false nếu thất bại.
 * ---------------------------------------------------------------------- */
bool maapi_load_schema(const char *host_arg, int port_arg,
                       schema_node_t **out_schema) {
    if (!out_schema) return false;

    /* Xác định host và port — dùng giá trị mặc định nếu không được cung cấp */
    const char *host = host_arg ? host_arg : MAAPI_DEFAULT_HOST;
    int port = (port_arg > 0) ? port_arg : MAAPI_DEFAULT_PORT;

    /* Chuẩn bị địa chỉ TCP socket */
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    inet_aton(host, &addr.sin_addr);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    /* Khởi tạo thư viện libconfd ở chế độ im lặng */
    confd_init("cli-netconf-c", stderr, CONFD_SILENT);

    /* Kết nối tới ConfD qua giao thức MAAPI IPC */
    if (maapi_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != CONFD_OK) {
        maapi_close(sock);
        return false;
    }

    /* Mở phiên người dùng (cần thiết trước khi thao tác MAAPI) */
    if (maapi_start_user_session(sock, "admin", "system", NULL, 0,
                                 CONFD_PROTO_TCP) != CONFD_OK) {
        maapi_close(sock);
        return false;
    }

    /* Tải tất cả schema (namespace) đã đăng ký với ConfD vào bộ nhớ tiến trình.
     * Sau bước này, có thể dùng confd_find_cs_root() để lấy cs_node tree. */
    if (confd_load_schemas((struct sockaddr *)&addr, sizeof(addr)) != CONFD_OK) {
        fprintf(stderr, "[maapi] confd_load_schemas failed: %s\n", confd_lasterr());
    }

    /* Lấy danh sách tất cả namespace đã tải */
    struct confd_nsinfo *ns_list = NULL;
    int ns_count = confd_get_nslist(&ns_list);
    if (ns_count <= 0) {
        maapi_end_user_session(sock);
        close(sock);
        return false;
    }

    /* Tạo root node nếu caller chưa cung cấp */
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

    /* Duyệt từng namespace và xây dựng schema tree.
     * Bỏ qua các namespace không phải cấu hình (NETCONF ops, monitoring,
     * notifications, thư viện nội bộ ConfD...) */
    for (int i = 0; i < ns_count; i++) {
        const char *uri = ns_list[i].uri;
        if (!uri) continue;

        /* Lọc bỏ các namespace hệ thống/nội bộ không chứa cấu hình */
        if (strstr(uri, "urn:ietf:params:xml:ns:netconf:") ||
            strstr(uri, "urn:ietf:params:xml:ns:netmod:")  ||
            strstr(uri, "ietf-netconf-monitoring")         ||
            strstr(uri, "ietf-netconf-nmda")               ||
            strstr(uri, "ietf-netconf-notifications")      ||
            strstr(uri, "ietf-subscribed-notifications")   ||
            strstr(uri, "ietf-yang-library")               ||
            strstr(uri, "tail-f.com/ns/netconf/")          ||
            strstr(uri, "tail-f.com/yang/confd-monitoring") ||
            strstr(uri, "tail-f.com/ns/kicker")            ||
            strstr(uri, "tail-f.com/ns/progress")          ||
            strstr(uri, "tail-f.com/ns/rollback")          ||
            strstr(uri, "netconf/extensions"))
            continue;

        /* Lấy cs_node gốc của namespace này */
        struct confd_cs_node *root_cs =
            confd_find_cs_root(ns_list[i].hash);
        if (!root_cs) continue;

        /* Duyệt từng node cấp cao nhất trong namespace */
        for (struct confd_cs_node *top = root_cs; top; top = top->next) {
            const char *top_name = confd_hash2str(top->tag);
            if (!top_name) continue;

            /* Bỏ qua node RPC/action (không có children = leaf/action, không phải config) */
            if (!top->children) continue;

            /* Tìm hoặc tạo schema node cấp cao nhất tương ứng */
            schema_node_t *sn = NULL;
            for (schema_node_t *c = root->children; c; c = c->next) {
                if (strcmp(c->name, top_name) == 0) { sn = c; break; }
            }
            if (!sn) {
                sn = schema_new_node(top_name);
                if (!sn) continue;
                /* Gắn namespace URI cho node cấp cao nhất */
                if (ns_list[i].uri)
                    strncpy(sn->ns, ns_list[i].uri, MAX_NS_LEN - 1);
                sn->next = root->children;
                root->children = sn;
            }

            /* Đệ quy duyệt toàn bộ cây con của namespace này */
            walk_cs_node(top, sn);
        }
    }

    /* Dọn dẹp: giải phóng danh sách namespace, đóng phiên và socket */
    free(ns_list);
    maapi_end_user_session(sock);
    maapi_close(sock);

    /* Đếm số node cấp cao nhất để ghi log (debug) */
    int top_count = 0;
    for (schema_node_t *c = root->children; c; c = c->next) top_count++;
    return true;
}

#endif /* WITH_MAAPI */
