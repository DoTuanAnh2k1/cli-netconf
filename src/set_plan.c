/*
 * set_plan.c — Pure planning layer cho lệnh `set`.
 * See include/set_plan.h for contract.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include "set_plan.h"

static schema_node_t *find_child(schema_node_t *node, const char *name) {
    if (!node) return NULL;
    for (schema_node_t *c = node->children; c; c = c->next) {
        if (strcasecmp(c->name, name) == 0) return c;
    }
    return NULL;
}

static void plan_set_err(set_plan_t *p, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    free(p->err);
    p->err = strdup(buf);
}

static int plan_add_op(set_plan_t *p, set_op_kind_t k,
                       const char *kp, const char *val) {
    if (p->n_ops >= p->cap) {
        int ncap = p->cap ? p->cap * 2 : 4;
        set_op_t *no = realloc(p->ops, (size_t)ncap * sizeof(*no));
        if (!no) return -1;
        p->ops = no;
        p->cap = ncap;
    }
    p->ops[p->n_ops].kind    = k;
    p->ops[p->n_ops].keypath = strdup(kp);
    p->ops[p->n_ops].value   = val ? strdup(val) : NULL;
    p->n_ops++;
    return 0;
}

static char *kp_append(char **kp, size_t *len, size_t *cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (need < 0) { va_end(ap); return NULL; }
    while (*len + (size_t)need + 1 >= *cap) {
        *cap *= 2;
        char *n = realloc(*kp, *cap);
        if (!n) { va_end(ap); return NULL; }
        *kp = n;
    }
    *len += (size_t)vsnprintf(*kp + *len, *cap - *len, fmt, ap);
    va_end(ap);
    return *kp;
}

void set_plan_free(set_plan_t *p) {
    if (!p) return;
    for (int i = 0; i < p->n_ops; i++) {
        free(p->ops[i].keypath);
        free(p->ops[i].value);
    }
    free(p->ops);
    free(p->list_kp);
    free(p->err);
    free(p);
}

static set_plan_t *plan_new(void) {
    return calloc(1, sizeof(set_plan_t));
}

/* Keypath direct mode ("/..."): trả về plan hoàn chỉnh hoặc err. */
static void plan_from_keypath(set_plan_t *p, char **args, int argc) {
    const char *path = args[0];
    size_t plen = strlen(path);

    if (argc >= 2) {
        if (argc > 2) {
            plan_set_err(p, "Keypath nhận 1 value, có thừa: '%s'", args[2]);
            return;
        }
        plan_add_op(p, SET_OP_SET_LEAF, path, args[1]);
        p->shape = SET_SHAPE_SINGLE_LEAF;
        return;
    }
    /* argc == 1 — nếu keypath trỏ đến list entry (kết thúc '}') → tạo entry. */
    if (plen > 0 && path[plen - 1] == '}') {
        plan_add_op(p, SET_OP_CREATE_ENTRY, path, NULL);
        p->shape = SET_SHAPE_KEY_ONLY;
        p->list_kp = strdup(path);
        return;
    }
    plan_set_err(p, "Keypath thiếu value: %s", path);
}

set_plan_t *plan_set(schema_node_t *schema, char **args, int argc) {
    set_plan_t *p = plan_new();
    if (!p) return NULL;

    if (!schema || !args || argc <= 0) {
        plan_set_err(p, "no args");
        return p;
    }

    /* Keypath direct. */
    if (args[0][0] == '/') {
        plan_from_keypath(p, args, argc);
        return p;
    }

    /* Space-separated — walk schema. */
    size_t cap = 512;
    char  *kp  = malloc(cap);
    if (!kp) { plan_set_err(p, "oom"); return p; }
    kp[0] = '\0';
    size_t len = 0;

    schema_node_t *node = schema;
    int i = 0;

    while (i < argc) {
        const char *token = args[i];

        schema_node_t *child = find_child(node, token);
        if (!child) {
            plan_set_err(p, "Unknown node '%s' at path %s",
                         token, *kp ? kp : "/");
            goto done;
        }

        kp_append(&kp, &len, &cap, "/%s", child->name);
        i++;

        /* LEAF: token kế là value, kết thúc. */
        if (child->is_leaf) {
            if (i >= argc) {
                plan_set_err(p, "Missing value for leaf '%s'", child->name);
                goto done;
            }
            if (i + 1 < argc) {
                plan_set_err(p, "Leaf '%s' nhận 1 value, có thừa: '%s'",
                             child->name, args[i + 1]);
                goto done;
            }
            plan_add_op(p, SET_OP_SET_LEAF, kp, args[i]);
            p->shape = SET_SHAPE_SINGLE_LEAF;
            goto done;
        }

        /* LIST: consume key, rồi quyết định batch / key-only / tiếp tục đi sâu. */
        if (child->is_list) {
            if (i >= argc) {
                plan_set_err(p, "Missing key value for list '%s'", child->name);
                goto done;
            }
            const char *key = args[i];
            kp_append(&kp, &len, &cap, "{%s}", key);
            i++;
            node = child;

            /* Không còn arg → KEY_ONLY. */
            if (i >= argc) {
                plan_add_op(p, SET_OP_CREATE_ENTRY, kp, NULL);
                p->shape   = SET_SHAPE_KEY_ONLY;
                p->list_kp = strdup(kp);
                goto done;
            }

            /* Peek token kế để quyết định batch vs tiếp tục đi sâu. */
            schema_node_t *peek = find_child(child, args[i]);
            if (peek && peek->is_leaf) {
                /* BATCH mode: remaining = cặp (leaf, value). */
                int remaining = argc - i;
                if (remaining % 2 != 0) {
                    plan_set_err(p,
                        "Cần số chẵn token (cặp <leaf> <value>), có %d",
                        remaining);
                    goto done;
                }
                /* Validate TẤT CẢ leaves trước khi add op — all-or-nothing. */
                for (int j = i; j + 1 < argc; j += 2) {
                    const char *lname = args[j];
                    schema_node_t *lc = find_child(child, lname);
                    if (!lc || !lc->is_leaf) {
                        plan_set_err(p,
                            "'%s' không phải leaf trong list '%s'",
                            lname, child->name);
                        goto done;
                    }
                    if (schema_is_key_leaf(child, lc->name)) {
                        plan_set_err(p,
                            "'%s' là key của list '%s' "
                            "— đã được set qua key value",
                            lc->name, child->name);
                        goto done;
                    }
                }
                /* Build ops: CREATE_ENTRY + N SET_LEAF. */
                plan_add_op(p, SET_OP_CREATE_ENTRY, kp, NULL);
                for (int j = i; j + 1 < argc; j += 2) {
                    size_t flen = strlen(kp) + 1 + strlen(args[j]) + 1;
                    char  *fp   = malloc(flen);
                    if (!fp) { plan_set_err(p, "oom"); goto done; }
                    snprintf(fp, flen, "%s/%s", kp, args[j]);
                    plan_add_op(p, SET_OP_SET_LEAF, fp, args[j + 1]);
                    free(fp);
                }
                p->shape   = SET_SHAPE_BATCH_LIST;
                p->list_kp = strdup(kp);
                goto done;
            }
            /* peek không phải leaf → container/list lồng, tiếp tục loop. */
            continue;
        }

        /* CONTAINER: đi sâu xuống. */
        node = child;
    }

    /* Hết loop mà không trả về → path chưa đủ. */
    plan_set_err(p, "Đường dẫn chưa đầy đủ — cần trỏ tới leaf hoặc list, at: %s",
                 *kp ? kp : "/");

done:
    free(kp);
    return p;
}
