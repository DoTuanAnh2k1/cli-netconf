/*
 * formatter.c — Chuyển đổi XML sang text và ngược lại, xử lý RPC reply (libxml2)
 *
 * File này cung cấp các hàm:
 *   - Kiểm tra và trích xuất thông tin từ RPC reply (ok, error, error-message)
 *   - Trích xuất phần dữ liệu <data> hoặc <config> từ XML response
 *   - Chuyển đổi XML cấu hình sang định dạng text thụt lề (dễ đọc cho CLI)
 *   - Chuyển đổi text cấu hình sang XML (để gửi qua NETCONF/MAAPI)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "cli.h"

/* -------------------------------------------------------------------------
 * Kiểm tra và trích xuất thông tin từ NETCONF RPC reply
 * ---------------------------------------------------------------------- */

/*
 * fmt_is_rpc_ok — Kiểm tra xem RPC reply có chứa <ok/> không
 *
 * ConfD trả về <ok/> khi thao tác thành công (commit, edit-config...).
 *
 * Tham số:
 *   reply — chuỗi XML RPC reply
 *
 * Trả về:
 *   true nếu reply chứa "<ok/>", false nếu không.
 */
bool fmt_is_rpc_ok(const char *reply) {
    return reply && strstr(reply, "<ok/>") != NULL;
}

/*
 * fmt_is_rpc_error — Kiểm tra xem RPC reply có chứa lỗi không
 *
 * ConfD trả về <rpc-error> khi thao tác thất bại.
 *
 * Tham số:
 *   reply — chuỗi XML RPC reply
 *
 * Trả về:
 *   true nếu reply chứa "<rpc-error>", false nếu không.
 */
bool fmt_is_rpc_error(const char *reply) {
    return reply && strstr(reply, "<rpc-error>") != NULL;
}

/*
 * fmt_extract_error_msg — Trích xuất thông báo lỗi từ RPC error reply
 *
 * Tìm nội dung bên trong thẻ <error-message>...</error-message>.
 * Nếu không tìm thấy, trả về chuỗi mặc định "rpc-error" hoặc "unknown error".
 *
 * Tham số:
 *   reply — chuỗi XML RPC reply chứa lỗi
 *
 * Trả về:
 *   Chuỗi thông báo lỗi (malloc'd, caller phải free).
 */
char *fmt_extract_error_msg(const char *reply) {
    if (!reply) return xstrdup("unknown error");

    /* Tìm thẻ mở <error-message...> */
    const char *start = strstr(reply, "<error-message");
    if (start) {
        /* Tìm dấu '>' kết thúc thẻ mở (có thể có thuộc tính xml:lang) */
        const char *gt = strchr(start, '>');
        if (gt) {
            gt++;
            /* Tìm thẻ đóng </error-message> */
            const char *end = strstr(gt, "</error-message>");
            if (end) {
                /* Trích xuất nội dung giữa thẻ mở và thẻ đóng */
                size_t len = (size_t)(end - gt);
                char *msg = malloc(len + 1);
                if (msg) { memcpy(msg, gt, len); msg[len] = '\0'; return msg; }
            }
        }
    }
    return xstrdup("rpc-error");
}

/* -------------------------------------------------------------------------
 * fmt_extract_data_xml — Trích xuất phần <data>...</data> nguyên dạng XML
 *
 * Tìm và trả về toàn bộ nội dung bao gồm cả thẻ bọc (<data> hoặc <config>).
 * Hỗ trợ cả format NETCONF (<data>) và MAAPI (<config>).
 *
 * Tham số:
 *   rpc_reply — chuỗi XML RPC reply đầy đủ
 *
 * Trả về:
 *   Chuỗi XML từ <data> đến </data> (malloc'd, caller phải free),
 *   hoặc NULL nếu không tìm thấy.
 * ---------------------------------------------------------------------- */
char *fmt_extract_data_xml(const char *rpc_reply) {
    if (!rpc_reply) return NULL;

    /* Thử tìm <data>, sau đó thử <config> (cho trường hợp MAAPI) */
    const char *start = strstr(rpc_reply, "<data>");
    const char *end_tag = "</data>";
    if (!start) {
        /* Thử <data có thuộc tính (ví dụ: <data xmlns="...">) */
        start = strstr(rpc_reply, "<data ");
    }
    if (!start) {
        /* Fallback: thử <config> (MAAPI trả về format này) */
        start = strstr(rpc_reply, "<config");
        end_tag = "</config>";
    }
    if (!start) return NULL;

    /* Tìm thẻ đóng tương ứng */
    const char *end = strstr(start, end_tag);
    if (!end) return NULL;
    end += strlen(end_tag);

    /* Sao chép đoạn XML từ thẻ mở đến hết thẻ đóng */
    size_t len = (size_t)(end - start);
    char *xml = malloc(len + 1);
    if (!xml) return NULL;
    memcpy(xml, start, len);
    xml[len] = '\0';
    return xml;
}

/*
 * fmt_extract_raw_data — Trích xuất nội dung bên trong <data> hoặc <config>
 *
 * Khác với fmt_extract_data_xml: hàm này chỉ trả về nội dung bên trong,
 * không bao gồm thẻ bọc. Ví dụ: "<data><system>...</system></data>"
 * → trả về "<system>...</system>"
 *
 * Tham số:
 *   rpc_reply — chuỗi XML RPC reply đầy đủ
 *
 * Trả về:
 *   Chuỗi XML nội dung bên trong (malloc'd, caller phải free),
 *   hoặc NULL nếu không tìm thấy.
 */
char *fmt_extract_raw_data(const char *rpc_reply) {
    if (!rpc_reply) return NULL;

    /* Tìm thẻ bọc: <data...> hoặc <config...> */
    const char *start = strstr(rpc_reply, "<data");
    const char *end_tag = "</data>";
    if (!start) {
        start = strstr(rpc_reply, "<config");
        end_tag = "</config>";
    }
    if (!start) return NULL;

    /* Tìm dấu '>' kết thúc thẻ mở */
    const char *gt = strchr(start, '>');
    if (!gt) return NULL;
    gt++; /* Di chuyển qua '>' để bắt đầu nội dung bên trong */

    /* Tìm thẻ đóng */
    const char *end = strstr(gt, end_tag);
    if (!end) return NULL;

    /* Sao chép phần nội dung bên trong (không bao gồm thẻ bọc) */
    size_t len = (size_t)(end - gt);
    char *raw = malloc(len + 1);
    if (!raw) return NULL;
    memcpy(raw, gt, len);
    raw[len] = '\0';
    return raw;
}

/* -------------------------------------------------------------------------
 * fmt_xml_to_text — Chuyển đổi XML cấu hình sang định dạng text thụt lề
 *
 * Tương tự hàm formatXMLResponse trong phiên bản Go.
 * Hỗ trợ lọc theo đường dẫn (path filter) để chỉ hiển thị một phần cây.
 * ---------------------------------------------------------------------- */

/*
 * strbuf_t — Bộ đệm chuỗi tự động mở rộng (string buffer)
 *
 * Dùng để xây dựng kết quả text dần dần mà không cần biết trước kích thước.
 */
typedef struct {
    char  *buf;  /* Con trỏ tới vùng nhớ chứa chuỗi */
    size_t len;  /* Độ dài hiện tại của chuỗi */
    size_t cap;  /* Dung lượng đã cấp phát */
} strbuf_t;

/*
 * sb_append — Thêm chuỗi vào cuối string buffer
 *
 * Tự động mở rộng buffer (nhân đôi) khi không đủ chỗ.
 *
 * Tham số:
 *   sb — con trỏ tới string buffer
 *   s  — chuỗi cần thêm vào
 */
static void sb_append(strbuf_t *sb, const char *s) {
    size_t slen = strlen(s);
    /* Mở rộng buffer nếu không đủ chỗ */
    while (sb->len + slen + 1 >= sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 4096;
        sb->buf = realloc(sb->buf, sb->cap);
        if (!sb->buf) return;
    }
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

/*
 * sb_printf — Thêm chuỗi đã format (printf-style) vào string buffer
 *
 * Sử dụng vsnprintf vào buffer tạm rồi gọi sb_append.
 *
 * Tham số:
 *   sb  — con trỏ tới string buffer
 *   fmt — chuỗi format (giống printf)
 *   ... — các tham số format
 */
static void sb_printf(strbuf_t *sb, const char *fmt, ...) {
    char tmp[MAX_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_append(sb, tmp);
}

/*
 * get_key_value — Lấy giá trị key leaf đầu tiên của một list entry
 *
 * Trong YANG list, element con đầu tiên thường là key leaf.
 * Hàm này lấy nội dung text của element con đầu tiên để hiển thị
 * cùng với tên list (ví dụ: "server 10.0.0.1").
 *
 * Tham số:
 *   node — node XML đại diện cho một list entry
 *
 * Trả về:
 *   Chuỗi giá trị key (malloc'd, caller phải free), hoặc NULL.
 */
/* Heuristic phát hiện list entry: có sibling cùng tên. Không có schema nên
 * list 1 phần tử sẽ bị nhận nhầm là container — chấp nhận được (key vẫn
 * hiện ở cấp dưới dưới dạng leaf). */
static bool is_list_entry(xmlNodePtr node) {
    const char *name = (const char *)node->name;
    for (xmlNodePtr s = node->next; s; s = s->next) {
        if (s->type == XML_ELEMENT_NODE && strcmp((char *)s->name, name) == 0)
            return true;
    }
    for (xmlNodePtr s = node->prev; s; s = s->prev) {
        if (s->type == XML_ELEMENT_NODE && strcmp((char *)s->name, name) == 0)
            return true;
    }
    return false;
}

static char *get_key_value(xmlNodePtr node) {
    if (!is_list_entry(node)) return NULL;
    /* Element con đầu tiên phải là leaf thuần (không có cháu element). */
    for (xmlNodePtr c = node->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        for (xmlNodePtr gc = c->children; gc; gc = gc->next) {
            if (gc->type == XML_ELEMENT_NODE) return NULL;
        }
        xmlChar *content = xmlNodeGetContent(c);
        if (!content) return NULL;
        char *val = xstrdup((char *)content);
        xmlFree(content);
        return val;
    }
    return NULL;
}

/*
 * is_key_leaf_of_list_entry — heuristic: leaf này có phải là key leaf của
 * 1 list entry không. Dùng để tô màu khác (cyan) với leaf thường (yellow).
 *
 * Quy tắc:
 *   - Cha là list entry (có sibling cùng tên).
 *   - Leaf là element con đầu tiên của cha (trong YANG list, key được đặt
 *     làm child đầu tiên khi ConfD serialize).
 */
static bool is_key_leaf_of_list_entry(xmlNodePtr leaf) {
    if (!leaf || !leaf->parent) return false;
    if (!is_list_entry(leaf->parent)) return false;
    for (xmlNodePtr c = leaf->parent->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        return c == leaf;  /* First element child */
    }
    return false;
}

/*
 * node_has_element_children — Kiểm tra xem node XML có element con không
 *
 * Dùng để phân biệt leaf (không có element con) và container/list (có element con).
 *
 * Tham số:
 *   node — node XML cần kiểm tra
 *
 * Trả về:
 *   true nếu có ít nhất một element con, false nếu không.
 */
static bool node_has_element_children(xmlNodePtr node) {
    for (xmlNodePtr c = node->children; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE) return true;
    return false;
}

/*
 * render_node — Hiển thị đệ quy một node XML thành dạng text thụt lề
 *
 * Xử lý 3 trường hợp:
 *   1. Path filter đang hoạt động: chỉ đi sâu vào node khớp đường dẫn
 *   2. Leaf node: hiển thị "tên    giá_trị" (căn chỉnh cột)
 *   3. Container/list: hiển thị "tên [key]" rồi đệ quy vào con
 *
 * Tham số:
 *   sb          — string buffer để ghi kết quả
 *   node        — node XML hiện tại
 *   indent      — mức thụt lề hiện tại (số cấp)
 *   first_pass  — cờ cho chế độ hiển thị đường dẫn đầy đủ (chưa dùng)
 *   path_filter — mảng tên để lọc đường dẫn (có thể NULL)
 *   path_depth  — số phần tử trong path_filter
 *   path_idx    — vị trí hiện tại đang so khớp trong path_filter
 */
/* Tính độ rộng lớn nhất của tên các leaf con trực tiếp (để căn value thẳng
 * hàng trong cùng một container). Chỉ tính leaf — container/list không có
 * value để align. */
static int compute_leaf_name_width(xmlNodePtr parent) {
    int max = 0;
    for (xmlNodePtr c = parent->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        if (node_has_element_children(c)) continue;
        int n = (int)strlen((char *)c->name);
        if (n > max) max = n;
    }
    return max;
}

/* Phát hiện leaf-list: node `c` là leaf (không có element children) VÀ có
 * anh/em cùng tên cũng là leaf. Trả true nếu là thành viên leaf-list. */
static bool is_leaflist_member(xmlNodePtr c) {
    if (node_has_element_children(c)) return false;
    const char *name = (const char *)c->name;
    xmlNodePtr parent = c->parent;
    if (!parent) return false;
    for (xmlNodePtr s = parent->children; s; s = s->next) {
        if (s == c || s->type != XML_ELEMENT_NODE) continue;
        if (strcmp((char *)s->name, name) != 0) continue;
        if (node_has_element_children(s)) continue;  /* mixed → YANG list, not leaf-list */
        return true;
    }
    return false;
}

/* Kiểm tra node `c` có phải lần xuất hiện đầu tiên của tên này trong parent
 * hay không. Dùng để chỉ emit leaf-list 1 lần tại vị trí đầu. */
static bool is_first_by_name(xmlNodePtr c) {
    const char *name = (const char *)c->name;
    xmlNodePtr parent = c->parent;
    if (!parent) return true;
    for (xmlNodePtr s = parent->children; s && s != c; s = s->next) {
        if (s->type == XML_ELEMENT_NODE && strcmp((char *)s->name, name) == 0)
            return false;
    }
    return true;
}

/* Emit tất cả giá trị của leaf-list (tất cả siblings cùng tên) inline dạng
 *   <pad>name    : [ v1 v2 v3 ... ]
 * Name vàng, nội dung cyan, giống leaf thường. */
static void emit_leaflist_inline(strbuf_t *sb, xmlNodePtr parent,
                                  const char *name, int indent,
                                  int name_width) {
    char pad[64] = {0};
    int pad_lvl = indent;
    if (pad_lvl > 60) pad_lvl = 60;
    for (int i = 0; i < pad_lvl; i++) pad[i] = '\t';

    int nl = (int)strlen(name);
    int fill = (name_width > nl) ? (name_width - nl) : 0;
    sb_printf(sb, "%s" COLOR_YELLOW "%s" COLOR_RESET, pad, name);
    for (int i = 0; i < fill; i++) sb_append(sb, " ");
    sb_append(sb, ": " COLOR_CYAN "[ ");
    for (xmlNodePtr s = parent->children; s; s = s->next) {
        if (s->type != XML_ELEMENT_NODE) continue;
        if (strcmp((char *)s->name, name) != 0) continue;
        if (node_has_element_children(s)) continue;
        xmlChar *content = xmlNodeGetContent(s);
        sb_printf(sb, "%s ", content ? (char *)content : "");
        xmlFree(content);
    }
    sb_append(sb, "]" COLOR_RESET "\n");
}


static void render_node(strbuf_t *sb, xmlNodePtr node,
                         int indent, bool first_pass,
                         const char **path_filter, int path_depth,
                         int path_idx, int name_width) {
    if (node->type != XML_ELEMENT_NODE) return;

    const char *name = (char *)node->name;

    /* Lọc theo đường dẫn: chỉ đi sâu vào node khớp path_filter */
    if (path_depth > 0 && path_idx < path_depth) {
        /* So sánh tên node với phần tử hiện tại trong bộ lọc (không phân biệt hoa thường) */
        if (strcasecmp(name, path_filter[path_idx]) != 0) return;
        /* Nếu đã khớp phần tử cuối cùng của path:
         *   - Leaf → render trực tiếp node này (để hiện "name: value")
         *   - Container/list → render children như bình thường (giữ semantic
         *     "show what's under this path", không in lại tên container).
         * Thiếu nhánh leaf ở đây là nguyên nhân `show running-config <leaf>`
         * không in giá trị. */
        if (path_idx + 1 == path_depth) {
            if (!node_has_element_children(node)) {
                render_node(sb, node, indent, first_pass, NULL, 0, 0, 0);
                return;
            }
            /* Container matched cuối path — descend nhưng dùng width riêng
             * của container này để align các leaf con ngang hàng. Gom
             * leaf-list thành 1 dòng bracket.
             *
             * Nếu node là list entry (có sibling cùng tên), đặt key leaf
             * (child đầu tiên) ở indent hiện tại và các leaf/container còn
             * lại thụt thêm 1 cấp — giúp mắt phân biệt từng entry khi trong
             * list có nhiều entry liên tiếp. */
            int w = compute_leaf_name_width(node);
            int is_entry = is_list_entry(node);
            int seen_first = 0;
            for (xmlNodePtr c = node->children; c; c = c->next) {
                if (c->type != XML_ELEMENT_NODE) continue;
                int ci = indent;
                if (is_entry && seen_first) ci = indent + 1;
                if (is_leaflist_member(c)) {
                    if (!is_first_by_name(c)) continue;
                    emit_leaflist_inline(sb, node, (char *)c->name,
                                         ci + 1, w);
                    seen_first = 1;
                    continue;
                }
                render_node(sb, c, ci, first_pass,
                            path_filter, path_depth, path_idx + 1, w);
                seen_first = 1;
            }
            return;
        }
        /* Chưa hết path → descend với cùng width (0 = chưa khớp) */
        for (xmlNodePtr c = node->children; c; c = c->next) {
            render_node(sb, c, indent, first_pass,
                        path_filter, path_depth, path_idx + 1, name_width);
        }
        return;
    }

    /* Đến đây là hiển thị node thực sự (đã qua bộ lọc hoặc không có bộ lọc).
     * 1 cấp = 1 ký tự TAB; cùng level → cùng số tab. */
    char pad[64] = {0};
    int pad_lvl = indent + 1;  /* +1 vì dòng đầu (path) đã in sẵn, tất cả nội dung thụt 1 tab */
    if (pad_lvl > 60) pad_lvl = 60;
    for (int i = 0; i < pad_lvl; i++) pad[i] = '\t';

    if (!node_has_element_children(node)) {
        /* Leaf: "\t*N <name>: <value>"
         *   - name: cyan nếu là key của list entry, vàng cho leaf thường
         *   - value: cyan
         * Pad spaces sau name để ":" rơi đúng cột name_width → value thẳng hàng. */
        xmlChar *content = xmlNodeGetContent(node);
        if (!first_pass) {
            int nl = (int)strlen(name);
            int fill = (name_width > nl) ? (name_width - nl) : 0;
            const char *name_color = is_key_leaf_of_list_entry(node)
                                     ? COLOR_CYAN : COLOR_YELLOW;
            sb_printf(sb, "%s%s%s" COLOR_RESET, pad, name_color, name);
            for (int i = 0; i < fill; i++) sb_append(sb, " ");
            sb_printf(sb, ": " COLOR_CYAN "%s" COLOR_RESET "\n",
                      content ? (char *)content : "");
        }
        xmlFree(content);
    } else {
        /* Container hoặc list entry:
         *   - Container name: trắng (nổi rõ hơn default trên dark bg).
         *   - List entry header: "<container-name> <key_value>" — name trắng,
         *     key_value cyan.
         * List entry: children thụt thêm 1 cấp so với container thường để
         * tách rõ phạm vi của entry khi có nhiều list cùng cấp. */
        char *key_val = get_key_value(node);
        int  child_indent = indent + 1;
        if (key_val) {
            sb_printf(sb, "%s" COLOR_WHITE "%s" COLOR_RESET
                          " " COLOR_CYAN "%s" COLOR_RESET "\n",
                      pad, name, key_val);
            free(key_val);
            child_indent = indent + 2;
        } else {
            sb_printf(sb, "%s" COLOR_WHITE "%s" COLOR_RESET "\n",
                      pad, name);
        }
        /* Pre-scan leaf con để align value trong container này. Gom
         * leaf-list (nhiều leaf cùng tên) thành 1 dòng bracket. */
        int w = compute_leaf_name_width(node);
        for (xmlNodePtr c = node->children; c; c = c->next) {
            if (c->type != XML_ELEMENT_NODE) continue;
            if (is_leaflist_member(c)) {
                if (!is_first_by_name(c)) continue;
                emit_leaflist_inline(sb, node, (char *)c->name,
                                     child_indent + 1, w);
                continue;
            }
            render_node(sb, c, child_indent, false, NULL, 0, 0, w);
        }
    }
}

/*
 * fmt_xml_to_text — Chuyển đổi XML cấu hình thành text thụt lề dễ đọc
 *
 * Phân tích XML bằng libxml2, tìm node <data> hoặc <config>,
 * rồi đệ quy hiển thị từng node con dưới dạng text.
 *
 * Hỗ trợ lọc theo đường dẫn: nếu path != NULL, chỉ hiển thị
 * nhánh cây khớp với đường dẫn cho trước.
 *
 * Tham số:
 *   xml_data — chuỗi XML cấu hình
 *   path     — mảng chuỗi đường dẫn để lọc (NULL = hiển thị tất cả)
 *   path_len — số phần tử trong mảng path
 *
 * Trả về:
 *   Chuỗi text đã format (malloc'd, caller phải free).
 *   Trả về chuỗi rỗng nếu không có dữ liệu.
 */
char *fmt_xml_to_text(const char *xml_data,
                       const char **path, int path_len) {
    if (!xml_data) return xstrdup("");

    /* Phân tích XML bằng libxml2 (tắt thông báo lỗi/cảnh báo) */
    xmlDocPtr doc = xmlReadMemory(xml_data, (int)strlen(xml_data),
                                  "nc.xml", NULL,
                                  XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return xstrdup("");

    strbuf_t sb = {NULL, 0, 0};

    /* Tìm node wrapper <data> hoặc <config> chứa dữ liệu cấu hình */
    xmlNodePtr data_node = NULL;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root) {
        /* Nếu root chính là <config> (format MAAPI) → dùng trực tiếp */
        if (strcmp((char *)root->name, "config") == 0) {
            data_node = root;
        } else {
            /* Format NETCONF: <rpc-reply><data>...</data></rpc-reply> */
            for (xmlNodePtr n = root->children; n; n = n->next) {
                if (n->type == XML_ELEMENT_NODE &&
                    (strcmp((char *)n->name, "data") == 0 ||
                     strcmp((char *)n->name, "config") == 0)) {
                    data_node = n;
                    break;
                }
            }
        }
    }
    /* Fallback: nếu không tìm thấy wrapper, dùng root element */
    if (!data_node) data_node = root;

    /* Duyệt và hiển thị từng node con cấp cao nhất */
    if (data_node) {
        int top_w = compute_leaf_name_width(data_node);
        for (xmlNodePtr c = data_node->children; c; c = c->next) {
            render_node(&sb, c, 0, false,
                        path, path_len, 0, top_w);
        }
    }

    xmlFreeDoc(doc);
    if (!sb.buf) return xstrdup("");
    return sb.buf; /* Caller phải free */
}

/* Escape XML special chars vào strbuf (value của <leaf>value</leaf>). */
static void sb_xml_escape(strbuf_t *sb, const char *s) {
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
            case '<':  sb_append(sb, "&lt;");  break;
            case '>':  sb_append(sb, "&gt;");  break;
            case '&':  sb_append(sb, "&amp;"); break;
            case '"':  sb_append(sb, "&quot;"); break;
            case '\'': sb_append(sb, "&apos;"); break;
            default: {
                char buf[2] = {*s, '\0'};
                sb_append(sb, buf);
                break;
            }
        }
    }
}

/* Tìm schema node con theo tên (case-insensitive). */
static schema_node_t *find_child_by_name(schema_node_t *parent, const char *name) {
    if (!parent) return NULL;
    for (schema_node_t *c = parent->children; c; c = c->next) {
        if (strcasecmp(c->name, name) == 0) return c;
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * fmt_text_to_xml — Chuyển output của `show running-config` thành XML
 *                   hợp lệ để nạp vào ConfD qua maapi_load_config.
 *
 * Format input chấp nhận (giống output của show running-config):
 *   - Dòng đầu (tuỳ chọn) không có tab = path header từ cmd_show
 *       eg: "eir nsmfPduSession"
 *     Các token trong header được mở thành container ngoài. Nếu gặp list
 *     (is_list) trước khi hết header thì dừng — list entry cụ thể sẽ do
 *     dòng content đảm nhiệm.
 *   - Các dòng tiếp theo bắt đầu bằng N tab (N >= 1):
 *       "<name>: <value>"       → leaf  → <name>value</name>
 *       "<name>"                 → container → mở <name>, push stack
 *       "<list-name> <keyval>"   → list entry → mở <list-name>; key_val bị
 *                                   bỏ qua vì key leaf sẽ xuất hiện lại
 *                                   dưới dạng "key_name: keyval" ở con của nó.
 *   - Dòng bắt đầu bằng "(" (trailing timing "(12ms, ...)") bị skip.
 *   - Dòng rỗng / toàn khoảng trắng bị skip.
 *
 * Namespace: node cấp cao nhất (parent = schema root) được gắn xmlns lấy
 * từ schema->children[*]->ns (đã populate bởi maapi_load_schema).
 *
 * Tham số:
 *   text   — chuỗi text config
 *   schema — schema tree (để resolve namespace cho top-level node).
 *            NULL cũng chạy, nhưng XML sẽ thiếu xmlns → ConfD reject.
 *
 * Trả về:
 *   Chuỗi XML (malloc'd, caller phải free). Chuỗi rỗng nếu input không
 *   có dòng content hợp lệ.
 * ---------------------------------------------------------------------- */
char *fmt_text_to_xml(const char *text, schema_node_t *schema) {
    if (!text) return NULL;

    strbuf_t sb = {NULL, 0, 0};
    char   **tags = calloc(MAX_PATH_DEPTH, sizeof(char *));
    if (!tags) return NULL;
    int depth = 0;        /* Tổng số thẻ mở trong stack */
    int base_depth = 0;   /* Số thẻ mở từ path header — offset cho content */
    bool header_done = false;

    char *dup = xstrdup(text);
    if (!dup) { free(tags); return NULL; }

    char *saveptr = NULL;
    for (char *line = strtok_r(dup, "\n", &saveptr);
         line != NULL;
         line = strtok_r(NULL, "\n", &saveptr)) {

        /* Strip trailing CR/space/tab */
        char *end = line + strlen(line);
        while (end > line &&
               (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
            *--end = '\0';
        }
        if (*line == '\0') continue;            /* Dòng rỗng */
        if (line[0] == '(') continue;           /* "(12ms, ...)" — timing footer */

        /* Đếm tab đầu dòng */
        int lvl = 0;
        while (line[lvl] == '\t') lvl++;
        char *content = line + lvl;
        while (*content == ' ') content++;      /* Tab rồi có thể có space thừa */
        if (!*content) continue;

        if (lvl == 0 && !header_done) {
            /* Path header — các token tách bằng space. Walk schema theo từng
             * token để biết đâu là container (pre-open) và đâu là list
             * (dừng — list entry cụ thể nằm ở content). */
            schema_node_t *cur = schema;
            char *ht_save = NULL;
            for (char *tok = strtok_r(content, " ", &ht_save);
                 tok != NULL;
                 tok = strtok_r(NULL, " ", &ht_save)) {
                schema_node_t *found = find_child_by_name(cur, tok);
                if (found && found->is_list) break;     /* list → dừng */
                if (cur == schema && found && found->ns[0]) {
                    sb_printf(&sb, "<%s xmlns=\"%s\">", tok, found->ns);
                } else {
                    sb_printf(&sb, "<%s>", tok);
                }
                tags[depth++] = xstrdup(tok);
                if (found) cur = found;
                if (depth >= MAX_PATH_DEPTH - 1) break;
            }
            base_depth = depth;
            header_done = true;
            continue;
        }

        header_done = true;                     /* Sau content, không còn header */

        int effective = base_depth + lvl;
        if (effective < 1) effective = 1;

        /* Đóng các thẻ thừa đến khi depth == effective - 1 (parent của dòng hiện tại) */
        while (depth > effective - 1 && depth > 0) {
            depth--;
            sb_printf(&sb, "</%s>", tags[depth]);
            free(tags[depth]);
            tags[depth] = NULL;
        }

        /* Phân loại dòng theo dấu ": " (leaf) hay space (list entry) */
        char *colon = strstr(content, ": ");
        if (colon) {
            /* Leaf: "name: value" */
            *colon = '\0';
            char *name = content;
            char *val  = colon + 2;
            /* Trim trailing space trong name */
            char *nend = colon;
            while (nend > name && (nend[-1] == ' ' || nend[-1] == '\t')) *--nend = '\0';
            sb_printf(&sb, "<%s>", name);
            sb_xml_escape(&sb, val);
            sb_printf(&sb, "</%s>", name);
            /* Leaf không push stack */
        } else {
            /* Container hoặc list entry: "name" hoặc "name keyval" */
            char *space = strchr(content, ' ');
            if (space) *space = '\0';           /* Bỏ qua key_val — leaf sẽ emit lại */
            char *name = content;
            if (!*name) continue;

            if (depth == 0) {
                /* Top-level → tra namespace từ schema */
                schema_node_t *sn = find_child_by_name(schema, name);
                if (sn && sn->ns[0]) {
                    sb_printf(&sb, "<%s xmlns=\"%s\">", name, sn->ns);
                } else {
                    sb_printf(&sb, "<%s>", name);
                }
            } else {
                sb_printf(&sb, "<%s>", name);
            }
            if (depth < MAX_PATH_DEPTH - 1) {
                tags[depth++] = xstrdup(name);
            }
        }
    }

    /* Đóng tất cả thẻ còn mở */
    while (depth > 0) {
        depth--;
        sb_printf(&sb, "</%s>", tags[depth]);
        free(tags[depth]);
    }

    free(tags);
    free(dup);
    if (!sb.buf) return xstrdup("");
    return sb.buf;
}
