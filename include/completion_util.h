/*
 * completion_util.h — Pure helpers cho tab-completion.
 *
 * Tách logic "walk schema theo token bên trái cursor" khỏi readline
 * callbacks trong main.c để unit-test được. Không phụ thuộc readline.
 */
#ifndef COMPLETION_UTIL_H
#define COMPLETION_UTIL_H

#include "cli.h"

/*
 * completion_parent_for — Xác định schema node "cha" tại vị trí cursor.
 *
 * @param schema  Gốc schema tree (thường = g_schema)
 * @param line    Dòng lệnh đầy đủ (không cần trim)
 * @param cursor  Vị trí cursor (0..strlen(line)). Chỉ các token bên TRÁI
 *                cursor được xét — cho phép Tab ở giữa dòng gợi ý đúng.
 *
 * Xử lý:
 *   - Bỏ qua token đầu (tên lệnh: show/set/unset/…)
 *   - Sau "show" bỏ thêm token thứ 2 (running-config/candidate-config)
 *   - Walk schema theo các token còn lại tới vị trí cursor
 *   - Token cuối nếu đang gõ dở (không theo sau bởi space) → bỏ (là partial)
 *
 * Trả về node schema ứng với vị trí cursor (caller KHÔNG free — nó thuộc
 * schema tree). Trả NULL nếu schema NULL; trả schema nếu line rỗng /
 * chỉ có command.
 */
schema_node_t *completion_parent_for(schema_node_t *schema,
                                     const char *line,
                                     int cursor);

#endif /* COMPLETION_UTIL_H */
