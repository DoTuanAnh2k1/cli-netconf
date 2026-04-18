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
static void render_node(strbuf_t *sb, xmlNodePtr node,
                         int indent, bool first_pass,
                         const char **path_filter, int path_depth,
                         int path_idx) {
    if (node->type != XML_ELEMENT_NODE) return;

    const char *name = (char *)node->name;

    /* Lọc theo đường dẫn: chỉ đi sâu vào node khớp path_filter */
    if (path_depth > 0 && path_idx < path_depth) {
        /* So sánh tên node với phần tử hiện tại trong bộ lọc (không phân biệt hoa thường) */
        if (strcasecmp(name, path_filter[path_idx]) != 0) return;
        /* Tìm thấy phần tử khớp → tiếp tục duyệt sâu hơn */
        for (xmlNodePtr c = node->children; c; c = c->next) {
            render_node(sb, c, indent, first_pass,
                        path_filter, path_depth, path_idx + 1);
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
        /* Leaf: "\t*N <name> <value>" */
        xmlChar *content = xmlNodeGetContent(node);
        if (first_pass) {
            /* (chưa dùng) */
        } else {
            sb_printf(sb, "%s%s %s\n",
                      pad, name,
                      content ? (char *)content : "");
        }
        xmlFree(content);
    } else {
        /* Container hoặc list entry */
        char *key_val = get_key_value(node);
        if (key_val) {
            sb_printf(sb, "%s%s %s\n", pad, name, key_val);
            free(key_val);
        } else {
            sb_printf(sb, "%s%s\n", pad, name);
        }
        for (xmlNodePtr c = node->children; c; c = c->next) {
            if (c->type == XML_ELEMENT_NODE)
                render_node(sb, c, indent + 1, false,
                            NULL, 0, 0);
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
        for (xmlNodePtr c = data_node->children; c; c = c->next) {
            render_node(&sb, c, 0, false,
                        path, path_len, 0);
        }
    }

    xmlFreeDoc(doc);
    if (!sb.buf) return xstrdup("");
    return sb.buf; /* Caller phải free */
}

/* -------------------------------------------------------------------------
 * fmt_text_to_xml — Chuyển đổi text cấu hình thành NETCONF XML
 *
 * Phân tích định dạng text thụt lề (2 dấu cách = 1 cấp) và tạo XML.
 * Mỗi dòng có thể là:
 *   - "container_name" → mở thẻ XML: <container_name>
 *   - "leaf_name giá_trị" → thẻ leaf: <leaf_name>giá_trị</leaf_name>
 *
 * Ví dụ đầu vào:
 *   system
 *     hostname new-name
 *
 * Kết quả XML:
 *   <system><hostname>new-name</hostname></system>
 *
 * Tham số:
 *   text   — chuỗi text cấu hình đầu vào
 *   schema — schema tree cho tra cứu namespace (hiện chưa dùng)
 *
 * Trả về:
 *   Chuỗi XML (malloc'd, caller phải free), hoặc NULL nếu lỗi.
 * ---------------------------------------------------------------------- */
char *fmt_text_to_xml(const char *text, schema_node_t *schema) {
    (void)schema; /* Dự phòng cho tra cứu namespace trong tương lai */
    if (!text) return NULL;

    strbuf_t sb  = {NULL, 0, 0};
    char   **tags = calloc(MAX_PATH_DEPTH, sizeof(char *)); /* Stack thẻ XML đang mở */
    int      depth = 0;  /* Độ sâu hiện tại (số thẻ đang mở) */

    char *dup  = xstrdup(text);
    char *line = strtok(dup, "\n");

    while (line) {
        /* Đếm số dấu cách đầu dòng để xác định mức thụt lề (2 spaces = 1 cấp) */
        int spaces = 0;
        while (line[spaces] == ' ') spaces++;
        int level = spaces / 2;

        /* Loại bỏ khoảng trắng đầu dòng */
        char *content = line + spaces;
        while (*content == '\t') content++;

        /* Bỏ qua dòng rỗng và dòng comment (#) */
        if (*content == '\0' || *content == '#') {
            line = strtok(NULL, "\n");
            continue;
        }

        /* Đóng các thẻ XML thừa khi mức thụt lề giảm (quay lại cấp cha) */
        while (depth > level) {
            depth--;
            sb_printf(&sb, "</%s>", tags[depth]);
            free(tags[depth]);
            tags[depth] = NULL;
        }

        /* Phân tích dòng: "tên giá_trị" hoặc "tên" */
        char *space = strchr(content, ' ');
        if (space) {
            /* Có dấu cách → leaf node với giá trị */
            *space = '\0';
            char *val = space + 1;
            while (*val == ' ') val++; /* Bỏ khoảng trắng thừa */
            sb_printf(&sb, "<%s>%s</%s>", content, val, content);
        } else {
            /* Không có dấu cách → container hoặc list (mở thẻ) */
            sb_printf(&sb, "<%s>", content);
            tags[depth] = xstrdup(content);
            depth++;
        }
        line = strtok(NULL, "\n");
    }

    /* Đóng tất cả thẻ XML còn đang mở */
    while (depth > 0) {
        depth--;
        sb_printf(&sb, "</%s>", tags[depth]);
        free(tags[depth]);
    }

    free(tags);
    free(dup);
    return sb.buf;
}
