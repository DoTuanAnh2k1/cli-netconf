/*
 * json_util.h — Minimal JSON escape + field extractors.
 *
 * Không phải full JSON parser. Dùng cho body mgt-svc API nhỏ gọn,
 * flat object, string/int fields.
 */
#ifndef JSON_UTIL_H
#define JSON_UTIL_H

/* json_escape — Escape chuỗi để nhúng vào JSON string literal.
 * Xử lý: " \ \n \r \t và mọi byte < 0x20 (bao gồm \b = 0x08).
 * Trả về malloc'd string (caller free). "" nếu s == NULL. */
char *json_escape(const char *s);

/* json_extract_string — Tìm field "key":"value" trong object JSON,
 * trả về value dạng chuỗi (malloc'd, caller free).
 * Parser đơn giản: match "<key>": "<value>" với handling của \" và \\.
 * KHÔNG giải mã escape sequences — JWT/token dùng base64url không chứa escape. */
char *json_extract_string(const char *json, const char *key);

/* json_extract_int — Tìm field "key": <int>, trả về số. def nếu không có. */
int json_extract_int(const char *json, const char *key, int def);

#endif /* JSON_UTIL_H */
