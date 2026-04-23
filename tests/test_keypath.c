/*
 * test_keypath.c — Unit tests cho args_to_keypath (maapi-ops.c)
 *
 * Regression focus:
 *   - Container chain: "system ntp" → "/system/ntp"
 *   - List with key: "system ntp server 10.0.0.1" → "/system/ntp/server{10.0.0.1}"
 *   - Leaf stops the walk: "system ntp server 10.0.0.1 prefer" → stop at prefer
 *   - Unknown token: tolerated (append as-is)
 */
#include "test_common.h"
#include "cli.h"

/* Build 1 schema minimal cho test. Caller free bằng schema_free(root). */
static schema_node_t *build_test_schema(void) {
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

    schema_node_t *prefer = schema_new_node("prefer");
    prefer->is_leaf = true;
    prefer->next = srv->children; srv->children = prefer;

    return root;
}

static void _run_simple_leaf(void) {
    schema_node_t *root = build_test_schema();
    char *a[] = { "system", "hostname" };
    int consumed = 0;
    char *kp = args_to_keypath(root, a, 2, &consumed);
    ASSERT_EQ_STR("/system/hostname", kp);
    ASSERT_EQ_INT(2, consumed);
    free(kp);
    schema_free(root);
}

static void _run_nested_container(void) {
    schema_node_t *root = build_test_schema();
    char *a[] = { "system", "ntp", "enabled" };
    int consumed = 0;
    char *kp = args_to_keypath(root, a, 3, &consumed);
    ASSERT_EQ_STR("/system/ntp/enabled", kp);
    ASSERT_EQ_INT(3, consumed);
    free(kp);
    schema_free(root);
}

static void _run_list_with_key(void) {
    schema_node_t *root = build_test_schema();
    char *a[] = { "system", "ntp", "server", "10.0.0.1" };
    int consumed = 0;
    char *kp = args_to_keypath(root, a, 4, &consumed);
    ASSERT_EQ_STR("/system/ntp/server{10.0.0.1}", kp);
    ASSERT_EQ_INT(4, consumed);
    free(kp);
    schema_free(root);
}

static void _run_list_key_then_leaf(void) {
    schema_node_t *root = build_test_schema();
    char *a[] = { "system", "ntp", "server", "10.0.0.1", "prefer" };
    int consumed = 0;
    char *kp = args_to_keypath(root, a, 5, &consumed);
    ASSERT_EQ_STR("/system/ntp/server{10.0.0.1}/prefer", kp);
    ASSERT_EQ_INT(5, consumed);
    free(kp);
    schema_free(root);
}

static void _run_unknown_node_tolerated(void) {
    /* Gặp token lạ → thêm vào path rồi dừng. Hỗ trợ augment chưa trong schema. */
    schema_node_t *root = build_test_schema();
    char *a[] = { "system", "unknown_node" };
    int consumed = 0;
    char *kp = args_to_keypath(root, a, 2, &consumed);
    ASSERT_EQ_STR("/system/unknown_node", kp);
    ASSERT_EQ_INT(2, consumed);
    free(kp);
    schema_free(root);
}

static void _run_empty_args(void) {
    schema_node_t *root = build_test_schema();
    int consumed = -1;
    char *kp = args_to_keypath(root, NULL, 0, &consumed);
    ASSERT_TRUE(kp == NULL);
    ASSERT_EQ_INT(0, consumed);
    schema_free(root);
}

static void _run_null_schema(void) {
    char *a[] = { "x" };
    int consumed = -1;
    char *kp = args_to_keypath(NULL, a, 1, &consumed);
    ASSERT_TRUE(kp == NULL);
    ASSERT_EQ_INT(0, consumed);
}

int main(void) {
    fprintf(stderr, "\n[test_keypath]\n");

    TEST_CASE(simple_leaf);
    TEST_CASE(nested_container);
    TEST_CASE(list_with_key);
    TEST_CASE(list_key_then_leaf);
    TEST_CASE(unknown_node_tolerated);
    TEST_CASE(empty_args);
    TEST_CASE(null_schema);

    TEST_REPORT_AND_EXIT();
}
