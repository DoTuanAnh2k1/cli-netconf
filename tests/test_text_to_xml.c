/*
 * test_text_to_xml.c — Unit tests cho fmt_text_to_xml
 *
 * Regression focus:
 *   - Paste output `show running-config` phải tái tạo XML hợp lệ để
 *     ConfD load_config chấp nhận.
 *   - Leaf "name: value" → <name>value</name>
 *   - Container 1 token → mở thẻ, children thụt 1 tab thêm
 *   - List entry "name keyval" → mở <name>; key leaf sẽ xuất hiện
 *     lại ở dòng con dạng "keyname: keyval"
 *   - Top-level node gắn xmlns lấy từ schema
 *   - Dòng "(12ms, ...)" bị skip (timing footer)
 */
#include "test_common.h"
#include "cli.h"

static void _run_leaf_only(void) {
    schema_node_t *root = schema_new_node("__root__");
    schema_node_t *sys  = schema_new_node("system");
    snprintf(sys->ns, sizeof(sys->ns), "urn:test");
    sys->next = root->children; root->children = sys;

    schema_node_t *host = schema_new_node("hostname");
    host->is_leaf = true;
    host->next = sys->children; sys->children = host;

    const char *text =
        "system\n"
        "\thostname: edge-01\n";
    char *xml = fmt_text_to_xml(text, root);
    ASSERT_TRUE(xml != NULL);
    ASSERT_CONTAINS(xml, "<system xmlns=\"urn:test\">");
    ASSERT_CONTAINS(xml, "<hostname>edge-01</hostname>");
    ASSERT_CONTAINS(xml, "</system>");
    free(xml);
    schema_free(root);
}

static void _run_nested_container(void) {
    schema_node_t *root = schema_new_node("__root__");
    schema_node_t *sys  = schema_new_node("system");
    snprintf(sys->ns, sizeof(sys->ns), "urn:test");
    sys->next = root->children; root->children = sys;
    schema_node_t *ntp = schema_new_node("ntp");
    ntp->next = sys->children; sys->children = ntp;
    schema_node_t *en = schema_new_node("enabled");
    en->is_leaf = true;
    en->next = ntp->children; ntp->children = en;

    const char *text =
        "system\n"
        "\tntp\n"
        "\t\tenabled: true\n";
    char *xml = fmt_text_to_xml(text, root);
    ASSERT_TRUE(xml != NULL);
    ASSERT_CONTAINS(xml, "<system xmlns=\"urn:test\">");
    ASSERT_CONTAINS(xml, "<ntp>");
    ASSERT_CONTAINS(xml, "<enabled>true</enabled>");
    ASSERT_CONTAINS(xml, "</ntp>");
    ASSERT_CONTAINS(xml, "</system>");
    free(xml);
    schema_free(root);
}

static void _run_skip_timing_footer(void) {
    schema_node_t *root = schema_new_node("__root__");
    schema_node_t *sys  = schema_new_node("system");
    snprintf(sys->ns, sizeof(sys->ns), "urn:test");
    sys->next = root->children; root->children = sys;
    schema_node_t *host = schema_new_node("hostname");
    host->is_leaf = true;
    host->next = sys->children; sys->children = host;

    const char *text =
        "system\n"
        "\thostname: edge-01\n"
        "(12ms, 2026-04-23 10:00:00)\n";
    char *xml = fmt_text_to_xml(text, root);
    ASSERT_TRUE(xml != NULL);
    ASSERT_NOT_CONTAINS(xml, "12ms");
    ASSERT_NOT_CONTAINS(xml, "2026");
    free(xml);
    schema_free(root);
}

static void _run_escape_xml_special(void) {
    schema_node_t *root = schema_new_node("__root__");
    schema_node_t *sys  = schema_new_node("system");
    snprintf(sys->ns, sizeof(sys->ns), "urn:t");
    sys->next = root->children; root->children = sys;
    schema_node_t *desc = schema_new_node("description");
    desc->is_leaf = true;
    desc->next = sys->children; sys->children = desc;

    const char *text =
        "system\n"
        "\tdescription: A & B <C>\n";
    char *xml = fmt_text_to_xml(text, root);
    ASSERT_TRUE(xml != NULL);
    /* & phải thành &amp;, < thành &lt; để XML hợp lệ */
    ASSERT_CONTAINS(xml, "A &amp; B &lt;C&gt;");
    ASSERT_NOT_CONTAINS(xml, "A & B <C>");
    free(xml);
    schema_free(root);
}

static void _run_empty_input(void) {
    schema_node_t *root = schema_new_node("__root__");
    char *xml = fmt_text_to_xml("", root);
    /* Cho phép "" hoặc NULL, miễn là không crash */
    if (xml) { ASSERT_EQ_STR("", xml); free(xml); }
    schema_free(root);
}

int main(void) {
    fprintf(stderr, "\n[test_text_to_xml]\n");

    TEST_CASE(leaf_only);
    TEST_CASE(nested_container);
    TEST_CASE(skip_timing_footer);
    TEST_CASE(escape_xml_special);
    TEST_CASE(empty_input);

    TEST_REPORT_AND_EXIT();
}
