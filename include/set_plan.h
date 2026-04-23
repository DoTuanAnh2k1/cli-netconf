/*
 * set_plan.h — Pure planning layer cho lệnh `set`.
 *
 * Tách validation + keypath building ra khỏi MAAPI execution để unit-test
 * được toàn bộ edge cases (unknown node, odd-pair batch, set lại key leaf,
 * key-only entry, v.v.) mà không cần ConfD runtime.
 *
 * cmd_set gọi plan_set(g_schema, args, argc) → nhận 1 set_plan_t rồi:
 *   - Nếu plan->err != NULL: in err, return
 *   - Ngược lại: loop plan->ops, gọi MAAPI tương ứng
 */
#ifndef SET_PLAN_H
#define SET_PLAN_H

#include "cli.h"

typedef enum {
    SET_OP_SET_LEAF,       /* maapi_set_value_str(keypath, value) */
    SET_OP_CREATE_ENTRY,   /* maapi_create_list_entry(keypath) */
} set_op_kind_t;

typedef struct {
    set_op_kind_t kind;
    char         *keypath;  /* malloc'd */
    char         *value;    /* malloc'd, NULL nếu CREATE_ENTRY */
} set_op_t;

/* Ý niệm "hình thái" của plan — cmd_set dùng để chọn format message. */
typedef enum {
    SET_SHAPE_SINGLE_LEAF,   /* 1 op SET_LEAF */
    SET_SHAPE_KEY_ONLY,      /* 1 op CREATE_ENTRY */
    SET_SHAPE_BATCH_LIST,    /* CREATE_ENTRY + N SET_LEAF, cùng 1 list entry */
} set_shape_t;

typedef struct {
    set_op_t    *ops;
    int          n_ops;
    int          cap;
    set_shape_t  shape;
    char        *list_kp;   /* Cho BATCH/KEY_ONLY: keypath của list entry (prefix chung) */
    char        *err;       /* NULL nếu plan hợp lệ */
} set_plan_t;

/* plan_set — Phân tích args + schema → set_plan_t.
 *
 * Luôn trả về pointer khác NULL. Nếu có lỗi validation → plan->err set.
 * Không bao giờ partial: hoặc plan đầy đủ ops (err NULL), hoặc err set
 * và ops rỗng.
 *
 * Không handle paste mode (argc == 0) — caller tự xử lý nhánh đó. */
set_plan_t *plan_set(schema_node_t *schema, char **args, int argc);

void set_plan_free(set_plan_t *p);

#endif /* SET_PLAN_H */
