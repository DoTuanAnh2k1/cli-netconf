/*
 * test_completion.c — Unit tests cho completion_parent_for()
 *
 * Regression focus: tab-completion phải hoạt động đúng KHI CURSOR Ở
 * GIỮA DÒNG (user xoá một phần đầu rồi Tab, hoặc dừng giữa path).
 *
 * Pure test — không đụng readline, không đụng ConfD.
 */
#include "test_common.h"
#include "cli.h"
#include "completion_util.h"

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

    /* smf > nsmfPduSession (list) để sát với use case m đưa ra */
    schema_node_t *smf = schema_new_node("smf");
    smf->next = root->children; root->children = smf;

    schema_node_t *nsmf = schema_new_node("nsmfPduSession");
    nsmf->is_list = true;
    nsmf->n_keys  = 1;
    snprintf(nsmf->keys[0], sizeof(nsmf->keys[0]), "id");
    nsmf->next = smf->children; smf->children = nsmf;

    schema_node_t *vsmf = schema_new_node("vsmf");
    vsmf->is_leaf = true;
    vsmf->next = nsmf->children; nsmf->children = vsmf;

    return root;
}

static int cursor_at(const char *line, const char *marker) {
    const char *p = strstr(line, marker);
    return p ? (int)(p - line) : -1;
}

/* ─── Cursor ở cuối dòng (baseline) ─────────────────────── */

static void _run_cursor_at_end_empty_returns_root(void) {
    schema_node_t *s = build_schema();
    schema_node_t *p = completion_parent_for(s, "", 0);
    ASSERT_EQ_STR("__root__", p->name);
    schema_free(s);
}

static void _run_cursor_at_end_after_set_space(void) {
    /* "set " — Tab ở đây gợi ý top-level schema (children của root) */
    schema_node_t *s = build_schema();
    schema_node_t *p = completion_parent_for(s, "set ", 4);
    ASSERT_EQ_STR("__root__", p->name);
    schema_free(s);
}

static void _run_cursor_at_end_after_set_system(void) {
    /* "set system " — Tab gợi ý children của system */
    schema_node_t *s = build_schema();
    schema_node_t *p = completion_parent_for(s, "set system ", 11);
    ASSERT_EQ_STR("system", p->name);
    schema_free(s);
}

static void _run_cursor_at_end_partial_word(void) {
    /* "set system h" — h đang gõ dở, parent vẫn là "system" */
    schema_node_t *s = build_schema();
    schema_node_t *p = completion_parent_for(s, "set system h", 12);
    ASSERT_EQ_STR("system", p->name);
    schema_free(s);
}

static void _run_cursor_at_end_after_show(void) {
    /* "show running-config " bỏ qua 2 token đầu */
    schema_node_t *s = build_schema();
    schema_node_t *p = completion_parent_for(s, "show running-config ", 20);
    ASSERT_EQ_STR("__root__", p->name);
    schema_free(s);
}

/* ─── Cursor ở giữa dòng (bug đã fix) ───────────────────── */

static void _run_cursor_at_start_of_line(void) {
    /* Line: "smf nsmfPduSession vsmf", cursor ở pos 0 → coi như dòng rỗng
     * → parent là root. (Thực tế main.c sẽ trigger cmd_generator riêng
     * khi start==0; test này verify completion_parent_for tách biệt OK.) */
    schema_node_t *s = build_schema();
    const char *line = "smf nsmfPduSession vsmf";
    schema_node_t *p = completion_parent_for(s, line, 0);
    ASSERT_EQ_STR("__root__", p->name);
    schema_free(s);
}

static void _run_cursor_mid_after_set(void) {
    /* Line: "set smf nsmfPduSession vsmf", cursor ngay sau "set ".
     * Trước fix: walk toàn dòng → parent = nsmfPduSession children.
     * Sau fix: chỉ xét "set " → parent = root. */
    schema_node_t *s = build_schema();
    const char *line = "set smf nsmfPduSession vsmf";
    int c = cursor_at(line, "smf nsmfPduSession");  /* = 4 */
    ASSERT_EQ_INT(4, c);
    schema_node_t *p = completion_parent_for(s, line, c);
    ASSERT_EQ_STR("__root__", p->name);
    schema_free(s);
}

static void _run_cursor_mid_after_set_smf(void) {
    /* cursor sau "set smf " → parent = smf (gợi ý nsmfPduSession) */
    schema_node_t *s = build_schema();
    const char *line = "set smf nsmfPduSession vsmf";
    int c = cursor_at(line, "nsmfPduSession");  /* = 8 */
    schema_node_t *p = completion_parent_for(s, line, c);
    ASSERT_EQ_STR("smf", p->name);
    schema_free(s);
}

static void _run_cursor_mid_partial_word(void) {
    /* "set smf nsmf...|PduSession vsmf", cursor giữa "nsmf" và "PduSession"
     * → cursor sau partial "nsmf" → parent = smf. */
    schema_node_t *s = build_schema();
    const char *line = "set smf nsmfPduSession vsmf";
    int c = cursor_at(line, "PduSession");  /* = 12 */
    schema_node_t *p = completion_parent_for(s, line, c);
    ASSERT_EQ_STR("smf", p->name);
    schema_free(s);
}

static void _run_cursor_mid_after_list_name_space(void) {
    /* "set smf nsmfPduSession |vsmf" — cursor sau "nsmfPduSession " (list).
     * Vị trí này user gõ key VALUE, không có schema gợi ý — nhưng helper
     * trả về node đã walk tới: nsmfPduSession. */
    schema_node_t *s = build_schema();
    const char *line = "set smf nsmfPduSession vsmf";
    int c = cursor_at(line, "vsmf");  /* = 23 */
    schema_node_t *p = completion_parent_for(s, line, c);
    ASSERT_EQ_STR("nsmfPduSession", p->name);
    schema_free(s);
}

static void _run_null_line_returns_schema(void) {
    schema_node_t *s = build_schema();
    schema_node_t *p = completion_parent_for(s, NULL, 0);
    ASSERT_TRUE(p == s);
    schema_free(s);
}

static void _run_null_schema_returns_null(void) {
    schema_node_t *p = completion_parent_for(NULL, "set", 3);
    ASSERT_TRUE(p == NULL);
}

static void _run_cursor_beyond_line_clamped(void) {
    /* Cursor > strlen → clamp về strlen, không crash. */
    schema_node_t *s = build_schema();
    schema_node_t *p = completion_parent_for(s, "set system ", 999);
    ASSERT_EQ_STR("system", p->name);
    schema_free(s);
}

int main(void) {
    fprintf(stderr, "\n[test_completion]\n");

    /* Cursor ở cuối dòng (baseline) */
    TEST_CASE(cursor_at_end_empty_returns_root);
    TEST_CASE(cursor_at_end_after_set_space);
    TEST_CASE(cursor_at_end_after_set_system);
    TEST_CASE(cursor_at_end_partial_word);
    TEST_CASE(cursor_at_end_after_show);

    /* Cursor ở giữa dòng — bug đã fix */
    TEST_CASE(cursor_at_start_of_line);
    TEST_CASE(cursor_mid_after_set);
    TEST_CASE(cursor_mid_after_set_smf);
    TEST_CASE(cursor_mid_partial_word);
    TEST_CASE(cursor_mid_after_list_name_space);

    /* Edge cases */
    TEST_CASE(null_line_returns_schema);
    TEST_CASE(null_schema_returns_null);
    TEST_CASE(cursor_beyond_line_clamped);

    TEST_REPORT_AND_EXIT();
}
