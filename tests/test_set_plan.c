/*
 * test_set_plan.c — Unit tests cho plan_set() (src/set_plan.c)
 *
 * Cover 16 edge cases của cmd_set (không cần ConfD runtime):
 *   - Leaf đơn giản (keypath + space-separated)
 *   - List key-only (keypath + space-separated) — bug đã gặp
 *   - List batch (CREATE + N SET_LEAF)
 *   - Validation errors: unknown node, missing value, odd pair,
 *     non-leaf trong batch, set lại key leaf, incomplete path, etc.
 */
#include "test_common.h"
#include "cli.h"
#include "set_plan.h"

static schema_node_t *build_schema(void) {
    schema_node_t *root = schema_new_node("__root__");

    schema_node_t *sys = schema_new_node("system");
    sys->next = root->children; root->children = sys;

    schema_node_t *host = schema_new_node("hostname");
    host->is_leaf = true;
    host->next = sys->children; sys->children = host;

    schema_node_t *ntp = schema_new_node("ntp");
    ntp->next = sys->children; sys->children = ntp;

    schema_node_t *en = schema_new_node("enabled");
    en->is_leaf = true;
    en->next = ntp->children; ntp->children = en;

    schema_node_t *srv = schema_new_node("server");
    srv->is_list = true;
    srv->n_keys  = 1;
    snprintf(srv->keys[0], sizeof(srv->keys[0]), "address");
    srv->next = ntp->children; ntp->children = srv;

    schema_node_t *addr = schema_new_node("address");
    addr->is_leaf = true;
    addr->next = srv->children; srv->children = addr;

    schema_node_t *pref = schema_new_node("prefer");
    pref->is_leaf = true;
    pref->next = srv->children; srv->children = pref;

    schema_node_t *ver = schema_new_node("version");
    ver->is_leaf = true;
    ver->next = srv->children; srv->children = ver;

    return root;
}

/* ─── Space-separated: leaf ─────────────────────────────── */

static void _run_space_simple_leaf(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "hostname", "edge-01" };
    set_plan_t *p = plan_set(s, a, 3);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(p->err == NULL);
    ASSERT_EQ_INT(1, p->n_ops);
    ASSERT_EQ_INT(SET_OP_SET_LEAF, p->ops[0].kind);
    ASSERT_EQ_STR("/system/hostname", p->ops[0].keypath);
    ASSERT_EQ_STR("edge-01", p->ops[0].value);
    ASSERT_EQ_INT(SET_SHAPE_SINGLE_LEAF, p->shape);
    set_plan_free(p);
    schema_free(s);
}

static void _run_space_nested_leaf(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "ntp", "enabled", "true" };
    set_plan_t *p = plan_set(s, a, 4);
    ASSERT_TRUE(p->err == NULL);
    ASSERT_EQ_INT(1, p->n_ops);
    ASSERT_EQ_STR("/system/ntp/enabled", p->ops[0].keypath);
    ASSERT_EQ_STR("true", p->ops[0].value);
    set_plan_free(p);
    schema_free(s);
}

/* ─── Space-separated: list key-only (bug m đã gặp) ─────── */

static void _run_space_key_only(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "ntp", "server", "10.0.0.1" };
    set_plan_t *p = plan_set(s, a, 4);
    ASSERT_TRUE(p->err == NULL);
    ASSERT_EQ_INT(1, p->n_ops);
    ASSERT_EQ_INT(SET_OP_CREATE_ENTRY, p->ops[0].kind);
    ASSERT_EQ_STR("/system/ntp/server{10.0.0.1}", p->ops[0].keypath);
    ASSERT_TRUE(p->ops[0].value == NULL);
    ASSERT_EQ_INT(SET_SHAPE_KEY_ONLY, p->shape);
    set_plan_free(p);
    schema_free(s);
}

/* ─── Space-separated: list batch mode ──────────────────── */

static void _run_space_batch_one_leaf(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "ntp", "server", "10.0.0.1", "prefer", "true" };
    set_plan_t *p = plan_set(s, a, 6);
    ASSERT_TRUE(p->err == NULL);
    ASSERT_EQ_INT(2, p->n_ops);    /* CREATE + 1 SET_LEAF */
    ASSERT_EQ_INT(SET_OP_CREATE_ENTRY, p->ops[0].kind);
    ASSERT_EQ_STR("/system/ntp/server{10.0.0.1}", p->ops[0].keypath);
    ASSERT_EQ_INT(SET_OP_SET_LEAF, p->ops[1].kind);
    ASSERT_EQ_STR("/system/ntp/server{10.0.0.1}/prefer", p->ops[1].keypath);
    ASSERT_EQ_STR("true", p->ops[1].value);
    ASSERT_EQ_INT(SET_SHAPE_BATCH_LIST, p->shape);
    set_plan_free(p);
    schema_free(s);
}

static void _run_space_batch_two_leaves(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "ntp", "server", "10.0.0.1",
                  "prefer", "true", "version", "4" };
    set_plan_t *p = plan_set(s, a, 8);
    ASSERT_TRUE(p->err == NULL);
    ASSERT_EQ_INT(3, p->n_ops);
    ASSERT_EQ_INT(SET_OP_CREATE_ENTRY, p->ops[0].kind);
    ASSERT_EQ_STR("/system/ntp/server{10.0.0.1}/prefer",  p->ops[1].keypath);
    ASSERT_EQ_STR("/system/ntp/server{10.0.0.1}/version", p->ops[2].keypath);
    set_plan_free(p);
    schema_free(s);
}

/* ─── Keypath direct mode ──────────────────────────────── */

static void _run_keypath_leaf(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "/system/hostname", "edge-01" };
    set_plan_t *p = plan_set(s, a, 2);
    ASSERT_TRUE(p->err == NULL);
    ASSERT_EQ_INT(1, p->n_ops);
    ASSERT_EQ_INT(SET_OP_SET_LEAF, p->ops[0].kind);
    ASSERT_EQ_STR("/system/hostname", p->ops[0].keypath);
    ASSERT_EQ_STR("edge-01", p->ops[0].value);
    set_plan_free(p);
    schema_free(s);
}

static void _run_keypath_key_only(void) {
    /* Fix mới: keypath kết thúc '}' không có value → tạo entry */
    schema_node_t *s = build_schema();
    char *a[] = { "/system/ntp/server{10.0.0.1}" };
    set_plan_t *p = plan_set(s, a, 1);
    ASSERT_TRUE(p->err == NULL);
    ASSERT_EQ_INT(1, p->n_ops);
    ASSERT_EQ_INT(SET_OP_CREATE_ENTRY, p->ops[0].kind);
    ASSERT_EQ_STR("/system/ntp/server{10.0.0.1}", p->ops[0].keypath);
    ASSERT_EQ_INT(SET_SHAPE_KEY_ONLY, p->shape);
    set_plan_free(p);
    schema_free(s);
}

/* ─── Validation errors ────────────────────────────────── */

static void _run_err_unknown_node(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "foobar" };
    set_plan_t *p = plan_set(s, a, 2);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "Unknown node");
    ASSERT_CONTAINS(p->err, "foobar");
    ASSERT_EQ_INT(0, p->n_ops);
    set_plan_free(p);
    schema_free(s);
}

static void _run_err_missing_leaf_value(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "hostname" };   /* Thiếu value */
    set_plan_t *p = plan_set(s, a, 2);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "Missing value");
    set_plan_free(p);
    schema_free(s);
}

static void _run_err_extra_leaf_value(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "hostname", "a", "b" };
    set_plan_t *p = plan_set(s, a, 4);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "nhận 1 value");
    set_plan_free(p);
    schema_free(s);
}

static void _run_err_batch_odd_pair(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "ntp", "server", "10.0.0.1",
                  "prefer", "true", "version" };  /* 3 tokens, odd */
    set_plan_t *p = plan_set(s, a, 7);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "số chẵn");
    set_plan_free(p);
    schema_free(s);
}

static void _run_err_set_key_leaf(void) {
    /* Set lại key leaf `address` trong batch mode phải fail */
    schema_node_t *s = build_schema();
    char *a[] = { "system", "ntp", "server", "10.0.0.1",
                  "address", "1.2.3.4" };
    set_plan_t *p = plan_set(s, a, 6);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "address");
    ASSERT_CONTAINS(p->err, "key");
    set_plan_free(p);
    schema_free(s);
}

static void _run_err_non_leaf_in_batch(void) {
    /* Trong batch mode, gặp token không phải leaf → không phải batch mode,
     * sẽ tiếp tục walk. Gặp token cuối cùng không descend được → incomplete.
     * Test này đảm bảo: nếu sau key có 1 container + leaf (nhánh continue) → OK.
     * Nhưng nếu sau key có leaf không tồn tại trong list → error "không phải leaf". */
    schema_node_t *s = build_schema();
    char *a[] = { "system", "ntp", "server", "10.0.0.1",
                  "prefer", "true", "foo", "bar" };
    set_plan_t *p = plan_set(s, a, 8);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "không phải leaf");
    set_plan_free(p);
    schema_free(s);
}

static void _run_err_missing_list_key(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "ntp", "server" };  /* Không có key value */
    set_plan_t *p = plan_set(s, a, 3);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "Missing key");
    set_plan_free(p);
    schema_free(s);
}

static void _run_err_incomplete_container_path(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "system", "ntp" };   /* container — không trỏ tới leaf/list */
    set_plan_t *p = plan_set(s, a, 2);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "chưa đầy đủ");
    set_plan_free(p);
    schema_free(s);
}

static void _run_err_keypath_no_value_not_list(void) {
    /* Keypath trỏ đến leaf nhưng không có value */
    schema_node_t *s = build_schema();
    char *a[] = { "/system/hostname" };
    set_plan_t *p = plan_set(s, a, 1);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "thiếu value");
    set_plan_free(p);
    schema_free(s);
}

static void _run_err_keypath_extra_value(void) {
    schema_node_t *s = build_schema();
    char *a[] = { "/system/hostname", "edge-01", "extra" };
    set_plan_t *p = plan_set(s, a, 3);
    ASSERT_TRUE(p->err != NULL);
    ASSERT_CONTAINS(p->err, "nhận 1 value");
    set_plan_free(p);
    schema_free(s);
}

/* ─── Edge cases ───────────────────────────────────────── */

static void _run_null_schema_returns_err(void) {
    char *a[] = { "system" };
    set_plan_t *p = plan_set(NULL, a, 1);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(p->err != NULL);
    set_plan_free(p);
}

static void _run_empty_args_returns_err(void) {
    schema_node_t *s = build_schema();
    set_plan_t *p = plan_set(s, NULL, 0);
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(p->err != NULL);
    set_plan_free(p);
    schema_free(s);
}

static void _run_free_null_is_safe(void) {
    set_plan_free(NULL);
    /* Không crash là passed. */
    ASSERT_TRUE(1);
}

int main(void) {
    fprintf(stderr, "\n[test_set_plan]\n");

    TEST_CASE(space_simple_leaf);
    TEST_CASE(space_nested_leaf);
    TEST_CASE(space_key_only);
    TEST_CASE(space_batch_one_leaf);
    TEST_CASE(space_batch_two_leaves);
    TEST_CASE(keypath_leaf);
    TEST_CASE(keypath_key_only);

    TEST_CASE(err_unknown_node);
    TEST_CASE(err_missing_leaf_value);
    TEST_CASE(err_extra_leaf_value);
    TEST_CASE(err_batch_odd_pair);
    TEST_CASE(err_set_key_leaf);
    TEST_CASE(err_non_leaf_in_batch);
    TEST_CASE(err_missing_list_key);
    TEST_CASE(err_incomplete_container_path);
    TEST_CASE(err_keypath_no_value_not_list);
    TEST_CASE(err_keypath_extra_value);

    TEST_CASE(null_schema_returns_err);
    TEST_CASE(empty_args_returns_err);
    TEST_CASE(free_null_is_safe);

    TEST_REPORT_AND_EXIT();
}
