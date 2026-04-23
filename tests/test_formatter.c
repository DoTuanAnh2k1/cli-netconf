/*
 * test_formatter.c — Unit tests cho fmt_xml_to_text trong formatter.c
 *
 * Regression focus:
 *   - Màu theo loại node: container trắng (97m), leaf vàng (33m),
 *     leaf value cyan (36m), key leaf trong list entry cyan (36m).
 *   - List entry children thụt thêm 1 cấp so với container thường.
 *   - Leaf-list gộp thành 1 dòng dạng bracket.
 *
 * Test không check full output byte-per-byte (dễ vỡ khi layout đổi),
 * thay vào đó check các ANSI sequence + key substring có/không xuất hiện.
 */
#include "test_common.h"
#include "cli.h"

static void _run_leaf_is_yellow(void) {
    const char *xml =
        "<config xmlns=\"x\">"
        "  <system>"
        "    <hostname>edge-01</hostname>"
        "  </system>"
        "</config>";
    char *out = fmt_xml_to_text(xml, NULL, 0);
    ASSERT_TRUE(out != NULL);
    /* Leaf name: \033[33m (YELLOW) */
    ASSERT_CONTAINS(out, "\033[33mhostname\033[0m");
    /* Leaf value: \033[36m (CYAN) */
    ASSERT_CONTAINS(out, "\033[36medge-01\033[0m");
    free(out);
}

static void _run_container_is_white(void) {
    const char *xml =
        "<config xmlns=\"x\">"
        "  <system>"
        "    <hostname>h1</hostname>"
        "  </system>"
        "</config>";
    char *out = fmt_xml_to_text(xml, NULL, 0);
    ASSERT_TRUE(out != NULL);
    /* Container "system" — trắng (\033[97m) */
    ASSERT_CONTAINS(out, "\033[97msystem\033[0m");
    free(out);
}

static void _run_list_key_leaf_is_cyan(void) {
    /* Bên trong list entry, child đầu tiên là key leaf → tên leaf phải
     * cyan (không vàng như leaf thường). */
    const char *xml =
        "<config xmlns=\"x\">"
        "  <ntp>"
        "    <server><address>10.0.0.1</address><prefer>true</prefer></server>"
        "    <server><address>10.0.0.2</address><prefer>false</prefer></server>"
        "  </ntp>"
        "</config>";
    /* Path-filter descend vào server để thấy children của list entry */
    const char *path[] = { "ntp", "server" };
    char *out = fmt_xml_to_text(xml, path, 2);
    ASSERT_TRUE(out != NULL);
    /* Key name "address" phải cyan */
    ASSERT_CONTAINS(out, "\033[36maddress\033[0m");
    /* Leaf thường "prefer" phải vàng */
    ASSERT_CONTAINS(out, "\033[33mprefer");
    free(out);
}

static void _run_list_entry_header_white_plus_cyan_value(void) {
    /* Header dạng "list-name <key_value>": name trắng, value cyan. */
    const char *xml =
        "<config xmlns=\"x\">"
        "  <ntp>"
        "    <server><address>10.0.0.1</address></server>"
        "    <server><address>10.0.0.2</address></server>"
        "  </ntp>"
        "</config>";
    char *out = fmt_xml_to_text(xml, NULL, 0);
    ASSERT_TRUE(out != NULL);
    ASSERT_CONTAINS(out, "\033[97mserver\033[0m");
    ASSERT_CONTAINS(out, "\033[36m10.0.0.1\033[0m");
    ASSERT_CONTAINS(out, "\033[36m10.0.0.2\033[0m");
    free(out);
}

static int count_tabs_before_name(const char *haystack, const char *name) {
    /* Tìm "name" trong output, đếm ngược \t liền trước. Dùng để verify
     * indent-level một cách không phụ thuộc vào ANSI escape. */
    const char *p = strstr(haystack, name);
    if (!p) return -1;
    /* Nhảy lùi qua các ký tự không phải \t hoặc \n — chính là ANSI escape */
    while (p > haystack && p[-1] != '\t' && p[-1] != '\n') p--;
    int tabs = 0;
    while (p > haystack && p[-1] == '\t') { tabs++; p--; }
    return tabs;
}

static void _run_list_entry_children_indent_plus_one(void) {
    /* List entry children thụt thêm 1 cấp so với leaf thường cùng cha.
     * "ntp" có leaf "enabled" và list "server". "enabled" nằm ở indent X,
     * server header ở indent X, nhưng children của server ở X+2 (thêm 1
     * level cho tab vừa "entry-extra indent"). */
    const char *xml =
        "<config xmlns=\"x\">"
        "  <ntp>"
        "    <enabled>true</enabled>"
        "    <server><address>10.0.0.1</address></server>"
        "    <server><address>10.0.0.2</address></server>"
        "  </ntp>"
        "</config>";
    char *out = fmt_xml_to_text(xml, NULL, 0);
    ASSERT_TRUE(out != NULL);

    int tabs_enabled = count_tabs_before_name(out, "enabled");
    int tabs_server  = count_tabs_before_name(out, "server");
    int tabs_address = count_tabs_before_name(out, "address");
    ASSERT_TRUE(tabs_enabled >= 0);
    ASSERT_TRUE(tabs_server >= 0);
    ASSERT_TRUE(tabs_address >= 0);
    /* enabled và server cùng cấp (siblings trong ntp) */
    ASSERT_EQ_INT(tabs_enabled, tabs_server);
    /* address thụt sâu hơn server 2 cấp: 1 vì là child, +1 vì list-entry extra */
    ASSERT_EQ_INT(tabs_server + 2, tabs_address);

    free(out);
}

static void _run_leaflist_inline_bracket(void) {
    /* 3 thẻ cùng tên leaf (không có element con) → gom thành 1 dòng [ ... ] */
    const char *xml =
        "<config xmlns=\"x\">"
        "  <acl>"
        "    <allow>10.0.0.1</allow>"
        "    <allow>10.0.0.2</allow>"
        "    <allow>10.0.0.3</allow>"
        "  </acl>"
        "</config>";
    char *out = fmt_xml_to_text(xml, NULL, 0);
    ASSERT_TRUE(out != NULL);
    ASSERT_CONTAINS(out, "[ ");
    ASSERT_CONTAINS(out, "10.0.0.1");
    ASSERT_CONTAINS(out, "10.0.0.3");
    ASSERT_CONTAINS(out, "]");
    free(out);
}

/* ─── Invariant check: mọi ANSI \033[...m phải được close bằng reset \033[0m
 * trong cùng dòng, không để leak màu sang dòng kế tiếp (terminal nhức mắt). */
static int ansi_balanced_per_line(const char *s) {
    int opens_this_line = 0;
    int resets_this_line = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') {
            /* Mỗi open phải có ít nhất 1 reset đi kèm. */
            if (opens_this_line > 0 && resets_this_line == 0) return 0;
            opens_this_line = resets_this_line = 0;
            continue;
        }
        if (*p == '\033' && p[1] == '[') {
            /* Đếm "\033[...m" — thô nhưng đủ cho invariant. */
            const char *q = p + 2;
            while (*q && *q != 'm') q++;
            if (*q != 'm') return 0;
            if (q - p == 3 && p[2] == '0') resets_this_line++;
            else                           opens_this_line++;
            p = q;
        }
    }
    return 1;
}

static void _run_ansi_balanced(void) {
    /* Mix rộng các loại node để kích hoạt mọi nhánh màu. */
    const char *xml =
        "<config xmlns=\"x\">"
        "  <system>"
        "    <hostname>edge-01</hostname>"
        "    <ntp>"
        "      <enabled>true</enabled>"
        "      <server><address>10.0.0.1</address><prefer>true</prefer></server>"
        "    </ntp>"
        "    <acl>"
        "      <allow>a</allow><allow>b</allow>"
        "    </acl>"
        "  </system>"
        "</config>";
    char *out = fmt_xml_to_text(xml, NULL, 0);
    ASSERT_TRUE(out != NULL);
    ASSERT_TRUE(ansi_balanced_per_line(out));
    free(out);
}

static void _run_leaflist_values_cyan(void) {
    /* Giá trị trong bracket leaf-list phải cyan. */
    const char *xml =
        "<config xmlns=\"x\">"
        "  <acl>"
        "    <allow>10.0.0.1</allow>"
        "    <allow>10.0.0.2</allow>"
        "  </acl>"
        "</config>";
    char *out = fmt_xml_to_text(xml, NULL, 0);
    ASSERT_TRUE(out != NULL);
    ASSERT_CONTAINS(out, "\033[36m[ 10.0.0.1 10.0.0.2 ]\033[0m");
    free(out);
}

static void _run_no_leaf_value_yellow_leak(void) {
    /* Leaf value (cyan) không được tô vàng nhầm. */
    const char *xml =
        "<config xmlns=\"x\">"
        "  <system><hostname>edge-01</hostname></system>"
        "</config>";
    char *out = fmt_xml_to_text(xml, NULL, 0);
    ASSERT_TRUE(out != NULL);
    /* "edge-01" phải đi kèm cyan, không xuất hiện "\033[33medge-01" */
    ASSERT_NOT_CONTAINS(out, "\033[33medge-01");
    ASSERT_CONTAINS(out, "\033[36medge-01\033[0m");
    free(out);
}

static void _run_container_not_yellow(void) {
    /* Container name không được vàng (chỉ leaf name vàng). */
    const char *xml =
        "<config xmlns=\"x\">"
        "  <system><ntp><enabled>true</enabled></ntp></system>"
        "</config>";
    char *out = fmt_xml_to_text(xml, NULL, 0);
    ASSERT_TRUE(out != NULL);
    ASSERT_NOT_CONTAINS(out, "\033[33msystem");
    ASSERT_NOT_CONTAINS(out, "\033[33mntp");
    ASSERT_CONTAINS(out, "\033[97msystem");
    ASSERT_CONTAINS(out, "\033[97mntp");
    free(out);
}

int main(void) {
    fprintf(stderr, "\n[test_formatter]\n");

    TEST_CASE(leaf_is_yellow);
    TEST_CASE(container_is_white);
    TEST_CASE(list_key_leaf_is_cyan);
    TEST_CASE(list_entry_header_white_plus_cyan_value);
    TEST_CASE(list_entry_children_indent_plus_one);
    TEST_CASE(leaflist_inline_bracket);
    TEST_CASE(ansi_balanced);
    TEST_CASE(leaflist_values_cyan);
    TEST_CASE(no_leaf_value_yellow_leak);
    TEST_CASE(container_not_yellow);

    TEST_REPORT_AND_EXIT();
}
