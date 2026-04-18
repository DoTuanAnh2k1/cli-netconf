/*
 * schema.c — Cây schema phục vụ tab completion và điều hướng cấu hình
 *
 * Cây schema được xây dựng từ 3 nguồn (ưu tiên theo thứ tự):
 *   1. MAAPI (libconfd) — khi biên dịch WITH_MAAPI và ConfD chạy trên máy cục bộ
 *   2. NETCONF get-schema (RFC 6022) — phân tích văn bản YANG
 *   3. Fallback: phân tích XML running-config
 *
 * Cây schema sử dụng danh sách liên kết đơn giản, đủ cho tab completion.
 * Không cần đầy đủ ngữ nghĩa YANG — chỉ cần biết container/list nào tồn tại
 * và node nào là leaf (để xác định khi nào cần nhập giá trị).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "cli.h"

/* -------------------------------------------------------------------------
 * Quản lý node trong schema tree
 * ---------------------------------------------------------------------- */

/*
 * schema_new_node — Tạo một schema node mới với tên cho trước
 *
 * Cấp phát bộ nhớ bằng calloc (tất cả trường khởi tạo về 0/NULL/false).
 * Tên node được sao chép vào trường name (giới hạn MAX_NAME_LEN ký tự).
 *
 * Tham số:
 *   name — tên node (ví dụ: "system", "hostname", "ntp")
 *
 * Trả về:
 *   Con trỏ tới schema_node_t mới, hoặc NULL nếu không đủ bộ nhớ.
 */
schema_node_t *schema_new_node(const char *name) {
    schema_node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    strncpy(n->name, name, MAX_NAME_LEN - 1);
    return n;
}

/*
 * schema_is_key_leaf — Kiểm tra leaf có phải là key của list không.
 * Chỉ có nghĩa khi list_node->is_list. So sánh case-insensitive.
 */
bool schema_is_key_leaf(const schema_node_t *list_node, const char *leaf_name) {
    if (!list_node || !leaf_name || !list_node->is_list) return false;
    for (int i = 0; i < list_node->n_keys; i++) {
        if (strcasecmp(list_node->keys[i], leaf_name) == 0) return true;
    }
    return false;
}

/*
 * schema_free — Giải phóng đệ quy toàn bộ cây schema
 *
 * Giải phóng node hiện tại, tất cả con (children) và anh em (next/sibling).
 * Sử dụng đệ quy hậu tố: giải phóng con trước, rồi anh em, rồi chính nó.
 *
 * Tham số:
 *   root — gốc cây schema cần giải phóng (có thể NULL)
 */
void schema_free(schema_node_t *root) {
    if (!root) return;
    schema_free(root->children);  /* Giải phóng toàn bộ cây con */
    schema_free(root->next);      /* Giải phóng các anh em kế tiếp */
    free(root);
}

/*
 * find_or_create_child — Tìm node con theo tên, tạo mới nếu chưa có
 *
 * Duyệt danh sách children của parent để tìm node có tên khớp.
 * Nếu không tìm thấy, tạo node mới và chèn vào đầu danh sách.
 *
 * Tham số:
 *   parent — node cha chứa danh sách con
 *   name   — tên node con cần tìm/tạo
 *
 * Trả về:
 *   Con trỏ tới node con (đã có hoặc vừa tạo), hoặc NULL nếu lỗi cấp phát.
 */
static schema_node_t *find_or_create_child(schema_node_t *parent,
                                            const char *name) {
    /* Tìm kiếm trong danh sách con hiện có */
    for (schema_node_t *c = parent->children; c; c = c->next) {
        if (strcmp(c->name, name) == 0) return c;
    }
    /* Không tìm thấy → tạo mới và chèn vào đầu danh sách */
    schema_node_t *n = schema_new_node(name);
    if (!n) return NULL;
    n->next = parent->children;
    parent->children = n;
    return n;
}

/* -------------------------------------------------------------------------
 * schema_lookup — Tìm kiếm node trong schema tree theo đường dẫn
 *
 * Duyệt cây schema theo mảng tên đường dẫn, đi sâu từng cấp.
 * Ví dụ: path = ["system", "ntp", "server"], depth = 3
 *         → tìm root → system → ntp → server
 *
 * Tham số:
 *   root  — gốc cây schema
 *   path  — mảng chuỗi tên các node trên đường dẫn
 *   depth — số phần tử trong mảng path
 *
 * Trả về:
 *   Con trỏ tới node tìm được, hoặc NULL nếu đường dẫn không tồn tại.
 * ---------------------------------------------------------------------- */
schema_node_t *schema_lookup(schema_node_t *root,
                              const char **path, int depth) {
    schema_node_t *node = root;
    for (int i = 0; i < depth; i++) {
        /* Bỏ qua phần tử rỗng trong đường dẫn */
        if (!path[i] || path[i][0] == '\0') continue;

        /* Tìm node con có tên khớp */
        schema_node_t *child = NULL;
        for (schema_node_t *c = node->children; c; c = c->next) {
            if (strcmp(c->name, path[i]) == 0) { child = c; break; }
        }
        if (!child) return NULL; /* Không tìm thấy → đường dẫn không hợp lệ */
        node = child;
    }
    return node;
}

/* -------------------------------------------------------------------------
 * schema_merge — Hợp nhất cây schema nguồn (src) vào đích (dst)
 *
 * Duyệt đệ quy các node con của src. Với mỗi node:
 *   - Nếu đã tồn tại trong dst (trùng tên): đệ quy merge vào đó
 *   - Nếu chưa có: tạo bản sao nông (shallow copy) và thêm vào dst
 *
 * Node đích (dst) luôn được ưu tiên — thuộc tính is_list, is_leaf
 * của node đã có không bị ghi đè.
 *
 * Tham số:
 *   dst — cây schema đích (sẽ được bổ sung thêm node)
 *   src — cây schema nguồn (chỉ đọc, không bị sửa đổi)
 * ---------------------------------------------------------------------- */
void schema_merge(schema_node_t *dst, schema_node_t *src) {
    if (!dst || !src) return;

    for (schema_node_t *sc = src->children; sc; sc = sc->next) {
        /* Tìm node tương ứng trong dst */
        schema_node_t *dc = NULL;
        for (schema_node_t *c = dst->children; c; c = c->next) {
            if (strcmp(c->name, sc->name) == 0) { dc = c; break; }
        }
        if (!dc) {
            /* Node chưa có trong dst → tạo bản sao nông và thêm vào */
            dc = schema_new_node(sc->name);
            if (!dc) continue;
            dc->is_list = sc->is_list;
            dc->is_leaf = sc->is_leaf;
            strncpy(dc->ns, sc->ns, MAX_NS_LEN - 1);
            dc->n_keys = sc->n_keys;
            for (int k = 0; k < sc->n_keys; k++) {
                strncpy(dc->keys[k], sc->keys[k], MAX_NAME_LEN - 1);
                dc->keys[k][MAX_NAME_LEN - 1] = '\0';
            }
            dc->next = dst->children;
            dst->children = dc;
        }
        /* Đệ quy merge các node con */
        schema_merge(dc, sc);
    }
}

/* -------------------------------------------------------------------------
 * schema_child_names — Lấy danh sách tên tất cả node con
 *
 * Dùng cho tab completion: trả về mảng tên các node con của node cho trước.
 *
 * Tham số:
 *   node  — node cha cần lấy danh sách con
 *   count — con trỏ output: số lượng tên trong mảng
 *
 * Trả về:
 *   Mảng chuỗi tên (caller phải free từng phần tử và mảng),
 *   hoặc NULL nếu không có node con.
 * ---------------------------------------------------------------------- */
char **schema_child_names(schema_node_t *node, int *count) {
    *count = 0;
    if (!node) return NULL;

    /* Đếm số node con */
    int n = 0;
    for (schema_node_t *c = node->children; c; c = c->next) n++;
    if (n == 0) return NULL;

    /* Cấp phát mảng chuỗi (thêm 1 cho NULL terminator) */
    char **names = calloc(n + 1, sizeof(char *));
    if (!names) return NULL;

    /* Sao chép tên từng node con vào mảng */
    int i = 0;
    for (schema_node_t *c = node->children; c; c = c->next) {
        names[i++] = xstrdup(c->name);
    }
    *count = n;
    return names;
}

/* -------------------------------------------------------------------------
 * schema_parse_xml — Xây dựng schema tree từ XML get-config response (libxml2)
 *
 * Phân tích XML cấu hình trả về từ NETCONF get-config hoặc MAAPI
 * để suy luận cấu trúc schema (container, list, leaf).
 * Cách phát hiện leaf: node XML không có element con.
 * ---------------------------------------------------------------------- */

/*
 * build_from_xml — Hàm đệ quy xây dựng schema từ cây XML (libxml2)
 *
 * Duyệt từng element con của xml_parent và tạo schema node tương ứng.
 * Namespace được ghi nhận cho các container cấp cao nhất (top-level).
 *
 * Tham số:
 *   xml_parent    — node XML cha đang được xử lý
 *   schema_parent — node schema cha tương ứng
 *   root          — gốc schema tree (để xác định top-level)
 */
static void build_from_xml(xmlNodePtr xml_parent, schema_node_t *schema_parent,
                            schema_node_t *root) {
    for (xmlNodePtr n = xml_parent->children; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;

        const char *name = (char *)n->name;
        /* Tìm hoặc tạo node con trong schema tree */
        schema_node_t *child = find_or_create_child(schema_parent, name);
        if (!child) continue;

        /* Ghi nhận namespace cho container cấp cao nhất */
        if (schema_parent == root && n->ns && n->ns->href) {
            strncpy(child->ns, (char *)n->ns->href, MAX_NS_LEN - 1);
        }

        /* Xác định leaf: node XML không có element con nào */
        bool has_elem_child = false;
        for (xmlNodePtr c = n->children; c; c = c->next) {
            if (c->type == XML_ELEMENT_NODE) { has_elem_child = true; break; }
        }
        child->is_leaf = !has_elem_child;

        /* Đệ quy vào các node con */
        build_from_xml(n, child, root);
    }
}

/*
 * schema_parse_xml — Phân tích chuỗi XML và tạo schema tree
 *
 * Xử lý cả format NETCONF (<rpc-reply><data>...</data></rpc-reply>)
 * và format MAAPI (<config>...</config>).
 *
 * Tham số:
 *   xml_data — chuỗi XML cần phân tích
 *
 * Trả về:
 *   Gốc schema tree mới (caller phải gọi schema_free), hoặc NULL nếu lỗi.
 */
schema_node_t *schema_parse_xml(const char *xml_data) {
    if (!xml_data) return NULL;

    /* Phân tích XML bằng libxml2 (tắt thông báo lỗi/cảnh báo) */
    xmlDocPtr doc = xmlReadMemory(xml_data, (int)strlen(xml_data),
                                  "nc.xml", NULL, XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return NULL;

    schema_node_t *root = schema_new_node("__root__");
    if (!root) { xmlFreeDoc(doc); return NULL; }

    /* Tìm node <data> trong rpc-reply — đây là wrapper chứa dữ liệu cấu hình */
    xmlNodePtr data_node = NULL;
    xmlNodePtr cur = xmlDocGetRootElement(doc);
    if (cur) {
        /* Duyệt con trực tiếp của root element để tìm <data> */
        for (xmlNodePtr n = cur->children; n; n = n->next) {
            if (n->type == XML_ELEMENT_NODE &&
                strcmp((char *)n->name, "data") == 0) {
                data_node = n;
                break;
            }
        }
    }
    /* Nếu không tìm thấy <data>, dùng root element làm điểm bắt đầu */
    if (!data_node) data_node = cur;

    /* Xây dựng schema tree từ XML */
    if (data_node) build_from_xml(data_node, root, root);
    xmlFreeDoc(doc);
    return root;
}

/* -------------------------------------------------------------------------
 * Bộ phân tích văn bản YANG — 2 bước (mở rộng grouping + xây dựng cây)
 * Chuyển đổi từ Go completer.go với cùng logic xử lý.
 *
 * Bước 1 (Pass 1): Thu thập tất cả grouping, phân tích nội dung từng grouping
 * Bước 2 (Pass 2): Xây dựng schema tree, mở rộng "uses" bằng grouping đã thu thập
 * ---------------------------------------------------------------------- */

/* Cấu trúc lưu trữ một grouping đã phân tích */
typedef struct grouping_entry {
    char              name[MAX_NAME_LEN];  /* Tên grouping (ví dụ: "vsmf-grp") */
    schema_node_t    *node;   /* Cây con (subtree) của grouping */
    struct grouping_entry *next;  /* Con trỏ tới grouping kế tiếp (danh sách liên kết) */
} grouping_entry_t;

/*
 * find_grouping — Tìm grouping theo tên trong danh sách liên kết
 *
 * Tham số:
 *   map  — đầu danh sách liên kết các grouping
 *   name — tên grouping cần tìm
 *
 * Trả về:
 *   Con trỏ tới grouping_entry_t nếu tìm thấy, NULL nếu không có.
 */
static grouping_entry_t *find_grouping(grouping_entry_t *map, const char *name) {
    for (grouping_entry_t *e = map; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

/*
 * free_groupings — Giải phóng toàn bộ danh sách grouping
 *
 * Giải phóng cả schema subtree của từng grouping và bản thân entry.
 *
 * Tham số:
 *   map — đầu danh sách liên kết cần giải phóng
 */
static void free_groupings(grouping_entry_t *map) {
    while (map) {
        grouping_entry_t *next = map->next;
        schema_free(map->node);
        free(map);
        map = next;
    }
}

/*
 * strip_prefix — Loại bỏ tiền tố module YANG khỏi tên
 *
 * Ví dụ: "smf:vsmf-grp" → "vsmf-grp", "hostname" → "hostname"
 *
 * Tham số:
 *   name — tên có thể chứa tiền tố "module:"
 *
 * Trả về:
 *   Con trỏ tới phần tên sau dấu ':', hoặc tên gốc nếu không có tiền tố.
 */
static const char *strip_prefix(const char *name) {
    const char *p = strrchr(name, ':');
    return p ? p + 1 : name;
}

/*
 * is_yang_meta — Kiểm tra xem dòng có phải là keyword metadata YANG không
 *
 * Các keyword metadata (namespace, prefix, type, revision...) không tạo
 * schema node mà chỉ là thông tin phụ trợ → cần bỏ qua khi xây dựng cây.
 *
 * Tham số:
 *   line — dòng YANG đã được trim khoảng trắng đầu
 *
 * Trả về:
 *   true nếu dòng bắt đầu bằng một keyword metadata YANG.
 */
static bool is_yang_meta(const char *line) {
    static const char *metas[] = {
        "namespace ", "prefix ", "key ", "type ", "revision ",
        "organization ", "description", "contact ", "default ",
        "range ", "length ", "ordered-by ", "nullable ", "enum ",
        "format ", "when ", "must ", "pattern ", "status ",
        "units ", "reference ", "if-feature ", "min-elements ",
        "max-elements ", "presence ", "config ", "fraction-digits ",
        "import ", "include ", "belongs-to ", "deviation ",
        "identity ", "extension ", "anyxml ", "anydata ",
        "rpc ", "notification ", "action ", "input ", "output ",
        NULL
    };
    for (int i = 0; metas[i]; i++) {
        if (strncmp(line, metas[i], strlen(metas[i])) == 0) return true;
    }
    return false;
}

/*
 * count_char — Đếm số lần xuất hiện của ký tự trong chuỗi
 *
 * Dùng để đếm dấu ngoặc nhọn '{' và '}' để theo dõi độ sâu lồng nhau.
 *
 * Tham số:
 *   s — chuỗi cần duyệt
 *   c — ký tự cần đếm
 *
 * Trả về:
 *   Số lần ký tự c xuất hiện trong chuỗi s.
 */
static int count_char(const char *s, char c) {
    int n = 0;
    while (*s) { if (*s == c) n++; s++; }
    return n;
}

/*
 * parse_grouping_body — Phân tích nội dung thân grouping thành schema node
 *
 * Xử lý văn bản YANG bên trong một khối grouping { ... }.
 * Hỗ trợ: container, list, leaf, leaf-list, uses (đệ quy mở rộng grouping).
 * Sử dụng stack để theo dõi node cha hiện tại khi đi sâu vào cây.
 *
 * Tham số:
 *   text      — nội dung văn bản YANG của grouping
 *   groupings — danh sách grouping đã phân tích (cho "uses" lồng nhau)
 *
 * Trả về:
 *   Schema node gốc chứa cây con của grouping (caller phải schema_free).
 */
static schema_node_t *parse_grouping_body(const char *text,
                                          grouping_entry_t *groupings) {
    schema_node_t *root  = schema_new_node("__grp__");
    schema_node_t *stack[MAX_PATH_DEPTH];  /* Stack theo dõi node cha hiện tại */
    int            depth = 0;
    stack[depth++] = root;
    int meta_depth = 0;  /* Theo dõi độ sâu khi đang trong khối metadata */

    char *text_copy = xstrdup(text);
    char *line = strtok(text_copy, "\n");
    while (line) {
        /* Loại bỏ khoảng trắng đầu và cuối dòng */
        while (*line == ' ' || *line == '\t') line++;
        char *end = line + strlen(line) - 1;
        while (end > line && (*end == ' ' || *end == '\t' || *end == '\r')) {
            *end-- = '\0';
        }

        /* Bỏ qua dòng rỗng */
        if (*line == '\0') { line = strtok(NULL, "\n"); continue; }

        /* Đếm số dấu ngoặc mở/đóng để theo dõi độ sâu */
        int opens  = count_char(line, '{');
        int closes = count_char(line, '}');

        /* Đang trong khối metadata → bỏ qua cho đến khi thoát khỏi khối */
        if (meta_depth > 0) {
            meta_depth += opens - closes;
            line = strtok(NULL, "\n"); continue;
        }

        /* Dấu đóng ngoặc đơn lẻ → lùi stack về node cha */
        if (strcmp(line, "}") == 0) {
            if (depth > 1) depth--;
            line = strtok(NULL, "\n"); continue;
        }

        /* Bỏ qua keyword metadata YANG (type, key, config...) */
        if (is_yang_meta(line)) {
            if (opens > closes) meta_depth += opens - closes;
            line = strtok(NULL, "\n"); continue;
        }

        /* Xử lý "uses <grouping-name>" → merge subtree của grouping vào node hiện tại */
        if (strncmp(line, "uses ", 5) == 0) {
            char gname[MAX_NAME_LEN];
            strncpy(gname, line + 5, MAX_NAME_LEN - 1);
            /* Loại bỏ ký tự thừa sau tên grouping */
            char *p = strpbrk(gname, " {;");
            if (p) *p = '\0';
            const char *bare = strip_prefix(gname);
            grouping_entry_t *g = find_grouping(groupings, bare);
            if (g) schema_merge(stack[depth - 1], g->node);
            line = strtok(NULL, "\n"); continue;
        }

        /* Xử lý các keyword tạo schema node: container, list, leaf-list, leaf */
        static const char *kws[] = {"container ", "list ", "leaf-list ", "leaf ", NULL};
        for (int k = 0; kws[k]; k++) {
            if (strncmp(line, kws[k], strlen(kws[k])) == 0) {
                /* Trích xuất tên node từ dòng */
                char name[MAX_NAME_LEN];
                const char *p = line + strlen(kws[k]);
                int ni = 0;
                while (*p && *p != ' ' && *p != '{' && *p != ';' && ni < MAX_NAME_LEN-1)
                    name[ni++] = *p++;
                name[ni] = '\0';

                schema_node_t *parent = stack[depth - 1];
                schema_node_t *child  = find_or_create_child(parent, name);
                if (child) {
                    /* Xác định loại node:
                     *   - list: keyword bắt đầu bằng "li" (list)
                     *   - leaf: keyword bắt đầu bằng "leaf" (leaf hoặc leaf-list) */
                    child->is_list = (kws[k][0] == 'l' && kws[k][1] == 'i');
                    child->is_leaf = (strncmp(kws[k], "leaf", 4) == 0);

                    /* Có dấu "{" ở cuối → đẩy node vào stack (đi sâu vào cây) */
                    if (line[strlen(line) - 1] == '{' && depth < MAX_PATH_DEPTH) {
                        stack[depth++] = child;
                    }
                }
                break;
            }
        }

        line = strtok(NULL, "\n");
    }
    free(text_copy);
    return root;
}

/* -------------------------------------------------------------------------
 * schema_parse_yang — Phân tích toàn bộ văn bản YANG và tạo schema tree
 *
 * Bộ phân tích 2 bước (2-pass parser):
 *   Bước 1: Thu thập tất cả khối "grouping" và phân tích nội dung
 *   Bước 2: Xây dựng schema tree chính, mở rộng "uses" và xử lý "augment"
 *
 * Tham số:
 *   yang_text — toàn bộ văn bản YANG module
 *   ns        — namespace URI để gắn cho node cấp cao nhất (có thể NULL)
 *
 * Trả về:
 *   Gốc schema tree mới (caller phải gọi schema_free), hoặc NULL nếu lỗi.
 * ---------------------------------------------------------------------- */
schema_node_t *schema_parse_yang(const char *yang_text, const char *ns) {
    if (!yang_text) return NULL;

    /* ═══ Bước 1: Thu thập tất cả grouping ═══
     * Duyệt qua văn bản YANG, tìm các khối "grouping <tên> { ... }",
     * trích xuất nội dung và phân tích thành schema subtree. */
    grouping_entry_t *groupings = NULL;
    {
        char *text = xstrdup(yang_text);
        char *p    = text;
        char *line;
        while ((line = strsep(&p, "\n")) != NULL) {
            /* Loại bỏ khoảng trắng đầu dòng */
            while (*line == ' ' || *line == '\t') line++;
            if (strncmp(line, "grouping ", 9) != 0) continue;

            /* Trích xuất tên grouping */
            char gname[MAX_NAME_LEN] = {0};
            const char *q = line + 9;
            int ni = 0;
            while (*q && *q != ' ' && *q != '{' && ni < MAX_NAME_LEN-1)
                gname[ni++] = *q++;
            gname[ni] = '\0';

            /* Thu thập nội dung thân grouping cho đến dấu "}" đóng tương ứng */
            int dep = count_char(line, '{') - count_char(line, '}');
            size_t body_cap = 4096, body_len = 0;
            char *body = malloc(body_cap);
            if (!body) continue;

            while (dep > 0 && p) {
                char *inner = strsep(&p, "\n");
                if (!inner) break;
                char *ti = inner;
                while (*ti == ' ' || *ti == '\t') ti++;
                int o = count_char(ti, '{');
                int c = count_char(ti, '}');
                dep += o - c;
                if (dep <= 0) break; /* Đã tìm thấy dấu "}" đóng → kết thúc */

                /* Mở rộng buffer nếu cần */
                size_t inner_len = strlen(inner);
                while (body_len + inner_len + 2 >= body_cap) {
                    body_cap *= 2;
                    body = realloc(body, body_cap);
                    if (!body) break;
                }
                if (!body) break;
                memcpy(body + body_len, inner, inner_len);
                body_len += inner_len;
                body[body_len++] = '\n';
                body[body_len] = '\0';
            }

            /* Tạo entry cho grouping và phân tích nội dung thành schema subtree */
            grouping_entry_t *entry = calloc(1, sizeof(*entry));
            if (entry) {
                strncpy(entry->name, gname, MAX_NAME_LEN - 1);
                entry->node = parse_grouping_body(body, groupings);
                entry->next = groupings;
                groupings   = entry;
            }
            free(body);
        }
        free(text);
    }

    /* ═══ Bước 2: Xây dựng schema tree chính ═══
     * Duyệt lại văn bản YANG, xử lý container/list/leaf/uses/augment,
     * bỏ qua các khối grouping (đã xử lý ở bước 1). */
    schema_node_t *root  = schema_new_node("__root__");
    schema_node_t *stack[MAX_PATH_DEPTH];  /* Stack theo dõi node cha hiện tại */
    int            depth = 0;
    stack[depth++] = root;

    int meta_depth    = 0;     /* Độ sâu khi đang trong khối metadata */
    int grp_depth     = 0;     /* Độ sâu khi đang trong khối grouping */
    bool in_grouping  = false; /* Cờ đánh dấu đang bỏ qua khối grouping */

    char *text = xstrdup(yang_text);
    char *p    = text;
    char *line;
    while ((line = strsep(&p, "\n")) != NULL) {
        /* Loại bỏ khoảng trắng đầu và cuối dòng */
        while (*line == ' ' || *line == '\t') line++;
        char *end = line + strlen(line) - 1;
        while (end > line && (*end == ' ' || *end == '\t' || *end == '\r')) *end-- = '\0';
        if (*line == '\0') continue;

        int opens  = count_char(line, '{');
        int closes = count_char(line, '}');

        /* Đang trong khối grouping → bỏ qua cho đến khi thoát */
        if (in_grouping) {
            grp_depth += opens - closes;
            if (grp_depth <= 0) in_grouping = false;
            continue;
        }

        /* Đang trong khối metadata → bỏ qua cho đến khi thoát */
        if (meta_depth > 0) {
            meta_depth += opens - closes;
            continue;
        }

        /* Dòng "module <tên> {" → bỏ qua (root đã là module) */
        if (strncmp(line, "module ", 7) == 0) {
            if (line[strlen(line)-1] == '{') stack[depth > 0 ? depth-1 : 0] = root;
            continue;
        }

        /* Gặp "grouping" → bật cờ bỏ qua (đã xử lý ở bước 1) */
        if (strncmp(line, "grouping ", 9) == 0) {
            in_grouping = true;
            grp_depth   = opens - closes;
            continue;
        }

        /* Xử lý "uses <grouping-name>" → merge subtree grouping vào node hiện tại */
        if (strncmp(line, "uses ", 5) == 0) {
            char gname[MAX_NAME_LEN];
            strncpy(gname, line + 5, MAX_NAME_LEN - 1);
            char *ep = strpbrk(gname, " {;");
            if (ep) *ep = '\0';
            const char *bare = strip_prefix(gname);
            grouping_entry_t *g = find_grouping(groupings, bare);
            if (g) schema_merge(stack[depth - 1], g->node);
            continue;
        }

        /* Xử lý "augment <đường-dẫn> {" → tìm/tạo node đích và đi sâu vào */
        if (strncmp(line, "augment ", 8) == 0) {
            /* Trích xuất đường dẫn augment từ dòng */
            char raw[MAX_LINE_LEN];
            strncpy(raw, line + 8, MAX_LINE_LEN - 1);
            char *ep = strpbrk(raw, " {");
            if (ep) *ep = '\0';

            /* Loại bỏ dấu nháy (nếu có) */
            char *path_str = raw;
            while (*path_str == '"' || *path_str == '\'') path_str++;
            char *path_end = path_str + strlen(path_str) - 1;
            while (path_end > path_str && (*path_end == '"' || *path_end == '\''))
                *path_end-- = '\0';

            /* Duyệt theo đường dẫn augment, tạo node nếu chưa có */
            schema_node_t *target = root;
            char *seg = strtok(path_str, "/");
            while (seg) {
                const char *bare_seg = strip_prefix(seg);
                if (*bare_seg) target = find_or_create_child(target, bare_seg);
                seg = strtok(NULL, "/");
            }
            /* Đẩy node đích vào stack để các dòng tiếp theo thêm vào đó */
            if (line[strlen(line)-1] == '{' && depth < MAX_PATH_DEPTH)
                stack[depth++] = target;
            continue;
        }

        /* Dấu "}" → lùi stack về node cha */
        if (strcmp(line, "}") == 0) {
            if (depth > 1) depth--;
            continue;
        }

        /* Bỏ qua keyword metadata YANG */
        if (is_yang_meta(line)) {
            if (opens > closes) meta_depth += opens - closes;
            continue;
        }

        /* Xử lý keyword tạo schema node: container, list, leaf-list, leaf */
        static const char *kws[] = {"container ", "list ", "leaf-list ", "leaf ", NULL};
        for (int k = 0; kws[k]; k++) {
            if (strncmp(line, kws[k], strlen(kws[k])) == 0) {
                /* Trích xuất tên node */
                char name[MAX_NAME_LEN];
                const char *q = line + strlen(kws[k]);
                int ni = 0;
                while (*q && *q != ' ' && *q != '{' && *q != ';' && ni < MAX_NAME_LEN-1)
                    name[ni++] = *q++;
                name[ni] = '\0';

                schema_node_t *parent = stack[depth - 1];
                schema_node_t *child  = find_or_create_child(parent, name);
                if (child) {
                    /* Xác định loại node theo keyword */
                    child->is_list = (kws[k][0] == 'l' && kws[k][1] == 'i');
                    child->is_leaf = (strncmp(kws[k], "leaf", 4) == 0);

                    /* Gắn namespace cho container cấp cao nhất (depth==1 = ngay dưới root) */
                    if (depth == 1 && ns && ns[0] && !child->ns[0])
                        strncpy(child->ns, ns, MAX_NS_LEN - 1);

                    /* Có dấu "{" ở cuối → đẩy node vào stack */
                    if (line[strlen(line)-1] == '{' && depth < MAX_PATH_DEPTH)
                        stack[depth++] = child;
                }
                break;
            }
        }
    }
    free(text);
    free_groupings(groupings);
    return root;
}



/* -------------------------------------------------------------------------
 * Các hàm tiện ích chung (Utility functions)
 * ---------------------------------------------------------------------- */

/*
 * xstrdup — Sao chép chuỗi an toàn (safe strdup)
 *
 * Giống strdup() nhưng xử lý trường hợp chuỗi nguồn NULL.
 * Sử dụng malloc + memcpy thay vì strdup() để đảm bảo tính di động.
 *
 * Tham số:
 *   s — chuỗi nguồn cần sao chép (có thể NULL)
 *
 * Trả về:
 *   Bản sao mới (malloc'd, caller phải free), hoặc NULL nếu s là NULL.
 */
char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char  *dup = malloc(len + 1);
    if (dup) memcpy(dup, s, len + 1);
    return dup;
}

/*
 * str_split — Tách chuỗi thành mảng token theo khoảng trắng
 *
 * Tách chuỗi input bằng dấu cách và tab. Mỗi token được sao chép
 * bằng xstrdup(). Mảng kết quả kết thúc bằng NULL.
 *
 * Tham số:
 *   s     — chuỗi input cần tách
 *   count — con trỏ output: số lượng token
 *
 * Trả về:
 *   Mảng chuỗi token (caller phải gọi free_tokens()), hoặc NULL nếu rỗng.
 */
char **str_split(const char *s, int *count) {
    *count = 0;
    if (!s) return NULL;
    char *dup = xstrdup(s);

    /* Bước 1: Đếm số token */
    int n = 0;
    char *p = dup;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;  /* Bỏ qua khoảng trắng */
        if (!*p) break;
        n++;
        while (*p && *p != ' ' && *p != '\t') p++;  /* Đi qua token */
    }
    if (n == 0) { free(dup); return NULL; }

    /* Bước 2: Cấp phát mảng và sao chép từng token */
    char **tokens = calloc(n + 1, sizeof(char *));
    p = dup; int i = 0;
    while (*p && i < n) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        *p = '\0'; p++;
        tokens[i++] = xstrdup(start);
    }
    free(dup);
    *count = i;
    return tokens;
}

/*
 * free_tokens — Giải phóng mảng token được tạo bởi str_split()
 *
 * Tham số:
 *   tokens — mảng chuỗi cần giải phóng
 *   count  — số lượng phần tử trong mảng
 */
void free_tokens(char **tokens, int count) {
    if (!tokens) return;
    for (int i = 0; i < count; i++) free(tokens[i]);
    free(tokens);
}

/*
 * str_trim — Loại bỏ khoảng trắng đầu và cuối chuỗi (sửa tại chỗ)
 *
 * Dịch con trỏ đầu qua các ký tự trắng, và ghi '\0' vào cuối
 * để cắt bỏ khoảng trắng phía sau.
 *
 * Tham số:
 *   s — chuỗi cần trim (sẽ bị sửa đổi tại chỗ)
 *
 * Trả về:
 *   Con trỏ tới ký tự đầu tiên không phải khoảng trắng.
 *   Lưu ý: con trỏ trả về có thể khác con trỏ đầu vào.
 */
char *str_trim(char *s) {
    /* Bỏ qua khoảng trắng đầu chuỗi */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    /* Cắt khoảng trắng cuối chuỗi */
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    return s;
}
