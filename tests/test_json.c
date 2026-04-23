/*
 * test_json.c — Unit tests cho json_util.c
 *
 * Regression focus:
 *   - json_escape phải xử lý mọi byte < 0x20, đặc biệt \b (0x08) →
 *     "" để mgt-svc decode được (bug đã gặp với PAM auth script).
 *   - json_extract_string phải nhận đúng key giữa nhiễu, bỏ qua key prefix
 *     trùng một phần.
 *   - json_extract_int trả default khi key không tồn tại.
 */
#include "test_common.h"
#include "json_util.h"

static void _run_escape_plain(void) {
    char *out = json_escape("hello");
    ASSERT_EQ_STR("hello", out);
    free(out);
}

static void _run_escape_quotes(void) {
    char *out = json_escape("say \"hi\"");
    ASSERT_EQ_STR("say \\\"hi\\\"", out);
    free(out);
}

static void _run_escape_backslash(void) {
    char *out = json_escape("a\\b");
    ASSERT_EQ_STR("a\\\\b", out);
    free(out);
}

static void _run_escape_newline_tab_cr(void) {
    char *out = json_escape("a\nb\tc\rd");
    ASSERT_EQ_STR("a\\nb\\tc\\rd", out);
    free(out);
}

static void _run_escape_backspace(void) {
    /* Bug gặp với mgt-svc: \b trong password bị gửi raw → JSON decode fail.
     * json_escape phải chuyển 0x08 →  (không có short form). */
    char in[] = { 'p', 'a', 's', 's', 0x08, 'w', 'd', '\0' };
    char *out = json_escape(in);
    ASSERT_EQ_STR("pass\\u0008wd", out);
    free(out);
}

static void _run_escape_all_ctrl(void) {
    char in[] = { 0x01, 0x1F, '\0' };
    char *out = json_escape(in);
    ASSERT_EQ_STR("\\u0001\\u001f", out);
    free(out);
}

static void _run_escape_null_input(void) {
    char *out = json_escape(NULL);
    ASSERT_EQ_STR("", out);
    free(out);
}

static void _run_extract_str_basic(void) {
    char *out = json_extract_string("{\"user\":\"alice\"}", "user");
    ASSERT_EQ_STR("alice", out);
    free(out);
}

static void _run_extract_str_with_spaces(void) {
    char *out = json_extract_string("{ \"user\" :  \"bob\" }", "user");
    ASSERT_EQ_STR("bob", out);
    free(out);
}

static void _run_extract_str_escape_handling(void) {
    /* Parser bỏ qua \" trong value để không dừng sai */
    char *out = json_extract_string(
        "{\"msg\":\"hello \\\"world\\\"\"}", "msg");
    /* Parser không giải mã escape — copy nguyên si */
    ASSERT_EQ_STR("hello \\\"world\\\"", out);
    free(out);
}

static void _run_extract_str_prefix_collision(void) {
    /* Key "user" không được khớp "username" */
    char *out = json_extract_string(
        "{\"username\":\"long\",\"user\":\"short\"}", "user");
    ASSERT_EQ_STR("short", out);
    free(out);
}

static void _run_extract_str_missing(void) {
    char *out = json_extract_string("{\"a\":\"x\"}", "b");
    ASSERT_TRUE(out == NULL);
}

static void _run_extract_str_null_input(void) {
    ASSERT_TRUE(json_extract_string(NULL, "key") == NULL);
    ASSERT_TRUE(json_extract_string("{}", NULL) == NULL);
}

static void _run_extract_int_basic(void) {
    int v = json_extract_int("{\"port\":4565}", "port", -1);
    ASSERT_EQ_INT(4565, v);
}

static void _run_extract_int_negative(void) {
    int v = json_extract_int("{\"n\":-7}", "n", 0);
    ASSERT_EQ_INT(-7, v);
}

static void _run_extract_int_missing(void) {
    int v = json_extract_int("{\"a\":1}", "b", 99);
    ASSERT_EQ_INT(99, v);
}

static void _run_extract_int_not_a_number(void) {
    /* Field có nhưng không phải số → fallback def */
    int v = json_extract_int("{\"x\":\"abc\"}", "x", 42);
    ASSERT_EQ_INT(42, v);
}

int main(void) {
    fprintf(stderr, "\n[test_json]\n");

    TEST_CASE(escape_plain);
    TEST_CASE(escape_quotes);
    TEST_CASE(escape_backslash);
    TEST_CASE(escape_newline_tab_cr);
    TEST_CASE(escape_backspace);
    TEST_CASE(escape_all_ctrl);
    TEST_CASE(escape_null_input);

    TEST_CASE(extract_str_basic);
    TEST_CASE(extract_str_with_spaces);
    TEST_CASE(extract_str_escape_handling);
    TEST_CASE(extract_str_prefix_collision);
    TEST_CASE(extract_str_missing);
    TEST_CASE(extract_str_null_input);

    TEST_CASE(extract_int_basic);
    TEST_CASE(extract_int_negative);
    TEST_CASE(extract_int_missing);
    TEST_CASE(extract_int_not_a_number);

    TEST_REPORT_AND_EXIT();
}
