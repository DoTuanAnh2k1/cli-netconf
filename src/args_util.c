/*
 * args_util.c — Schema-driven keypath helpers (không phụ thuộc ConfD lib).
 *
 * Tách ra khỏi maapi-ops.c để có thể unit-test mà không cần linker
 * kéo cả libconfd.so.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cli.h"
#include "maapi-direct.h"

/*
 * args_to_keypath — Chuyển đổi mảng token (từ dòng lệnh) thành keypath ConfD
 *
 * Duyệt schema tree để xác định loại từng node:
 *   - Container/leaf: thêm "/<tên>" vào keypath
 *   - List: thêm "/<tên>{<key>}" (token kế tiếp là giá trị key)
 *   - Leaf: dừng lại (phần còn lại trong args là value)
 *
 * Ví dụ:
 *   Input:  args = ["system", "ntp", "server", "10.0.0.1", "prefer"]
 *   Output: "/system/ntp/server{10.0.0.1}/prefer" (consumed = 5)
 */
char *args_to_keypath(schema_node_t *schema,
                      char **args, int argc,
                      int *consumed) {
    if (consumed) *consumed = 0;
    if (!schema || !args || argc == 0) return NULL;

    size_t cap = 512;
    char  *kp  = malloc(cap);
    if (!kp) return NULL;
    kp[0] = '\0';
    size_t len = 0;

    schema_node_t *node = schema;
    int i = 0;

    while (i < argc) {
        const char *token = args[i];

        schema_node_t *child = NULL;
        for (schema_node_t *c = node->children; c; c = c->next) {
            if (strcasecmp(c->name, token) == 0) { child = c; break; }
        }

        if (!child) {
            /* Không tìm thấy token trong schema — vẫn thêm vào keypath để hỗ trợ
             * các node chưa có trong schema (ví dụ: augment chưa load). */
            size_t needed = len + 1 + strlen(token) + 1;
            while (needed >= cap) { cap *= 2; kp = realloc(kp, cap); }
            len += (size_t)snprintf(kp + len, cap - len, "/%s", token);
            i++;
            if (consumed) *consumed = i;
            break;
        }

        size_t needed = len + 1 + strlen(child->name) + 1;
        while (needed >= cap) { cap *= 2; kp = realloc(kp, cap); }
        len += (size_t)snprintf(kp + len, cap - len, "/%s", child->name);
        i++;

        if (child->is_list && i < argc) {
            const char *key = args[i];
            size_t kneeded = len + 1 + strlen(key) + 2;
            while (kneeded >= cap) { cap *= 2; kp = realloc(kp, cap); }
            len += (size_t)snprintf(kp + len, cap - len, "{%s}", key);
            i++;
        }

        if (consumed) *consumed = i;

        if (child->is_leaf) break;

        node = child;
    }

    return kp;
}
