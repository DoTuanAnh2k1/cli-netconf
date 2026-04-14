/*
 * formatter.c — XML ↔ text conversion, RPC reply helpers (libxml2)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "cli.h"

/* -------------------------------------------------------------------------
 * fmt_is_rpc_ok / fmt_is_rpc_error / fmt_extract_error_msg
 * ---------------------------------------------------------------------- */
bool fmt_is_rpc_ok(const char *reply) {
    return reply && strstr(reply, "<ok/>") != NULL;
}

bool fmt_is_rpc_error(const char *reply) {
    return reply && strstr(reply, "<rpc-error>") != NULL;
}

char *fmt_extract_error_msg(const char *reply) {
    if (!reply) return xstrdup("unknown error");
    const char *start = strstr(reply, "<error-message");
    if (start) {
        const char *gt = strchr(start, '>');
        if (gt) {
            gt++;
            const char *end = strstr(gt, "</error-message>");
            if (end) {
                size_t len = (size_t)(end - gt);
                char *msg = malloc(len + 1);
                if (msg) { memcpy(msg, gt, len); msg[len] = '\0'; return msg; }
            }
        }
    }
    return xstrdup("rpc-error");
}

/* -------------------------------------------------------------------------
 * fmt_extract_data_xml — lấy phần <data>...</data> dạng chuỗi XML
 * ---------------------------------------------------------------------- */
char *fmt_extract_data_xml(const char *rpc_reply) {
    if (!rpc_reply) return NULL;
    const char *start = strstr(rpc_reply, "<data>");
    if (!start) {
        start = strstr(rpc_reply, "<data ");
        if (!start) return NULL;
    }
    const char *end = strstr(rpc_reply, "</data>");
    if (!end) return NULL;
    end += strlen("</data>");
    size_t len = (size_t)(end - start);
    char *xml = malloc(len + 1);
    if (!xml) return NULL;
    memcpy(xml, start, len);
    xml[len] = '\0';
    return xml;
}

/* Lấy nội dung bên trong <data>...</data> */
char *fmt_extract_raw_data(const char *rpc_reply) {
    if (!rpc_reply) return NULL;
    const char *start = strstr(rpc_reply, "<data");
    if (!start) return NULL;
    const char *gt = strchr(start, '>');
    if (!gt) return NULL;
    gt++;
    const char *end = strstr(gt, "</data>");
    if (!end) return NULL;
    size_t len = (size_t)(end - gt);
    char *raw = malloc(len + 1);
    if (!raw) return NULL;
    memcpy(raw, gt, len);
    raw[len] = '\0';
    return raw;
}

/* -------------------------------------------------------------------------
 * fmt_xml_to_text — recursive XML → text indent (libxml2)
 * Tương tự Go formatXMLResponse
 * ---------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} strbuf_t;

static void sb_append(strbuf_t *sb, const char *s) {
    size_t slen = strlen(s);
    while (sb->len + slen + 1 >= sb->cap) {
        sb->cap = sb->cap ? sb->cap * 2 : 4096;
        sb->buf = realloc(sb->buf, sb->cap);
        if (!sb->buf) return;
    }
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

static void sb_printf(strbuf_t *sb, const char *fmt, ...) {
    char tmp[MAX_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    sb_append(sb, tmp);
}

/* Lấy key leaf value của list entry */
static char *get_key_value(xmlNodePtr node) {
    /* Lấy text content của element con đầu tiên (thường là key) */
    for (xmlNodePtr c = node->children; c; c = c->next) {
        if (c->type == XML_ELEMENT_NODE) {
            xmlChar *content = xmlNodeGetContent(c);
            if (content) {
                char *val = xstrdup((char *)content);
                xmlFree(content);
                return val;
            }
        }
    }
    return NULL;
}

static bool node_has_element_children(xmlNodePtr node) {
    for (xmlNodePtr c = node->children; c; c = c->next)
        if (c->type == XML_ELEMENT_NODE) return true;
    return false;
}

static void render_node(strbuf_t *sb, xmlNodePtr node,
                         int indent, bool first_pass,
                         const char **path_filter, int path_depth,
                         int path_idx) {
    if (node->type != XML_ELEMENT_NODE) return;

    const char *name = (char *)node->name;

    /* Path filter */
    if (path_depth > 0 && path_idx < path_depth) {
        if (strcasecmp(name, path_filter[path_idx]) != 0) return;
        /* Found path element — continue deeper */
        for (xmlNodePtr c = node->children; c; c = c->next) {
            render_node(sb, c, indent, first_pass,
                        path_filter, path_depth, path_idx + 1);
        }
        return;
    }

    /* Đến đây là render thực sự */
    char pad[128] = {0};
    for (int i = 0; i < indent && i < 60; i++) pad[i] = ' ';

    if (!node_has_element_children(node)) {
        /* Leaf */
        xmlChar *content = xmlNodeGetContent(node);
        if (first_pass) {
            /* Path header mode: print full path + value */
        } else {
            sb_printf(sb, "%-*s%-24s %s\n",
                      indent * 2, "",
                      name,
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

char *fmt_xml_to_text(const char *xml_data,
                       const char **path, int path_len) {
    if (!xml_data) return xstrdup("");

    xmlDocPtr doc = xmlReadMemory(xml_data, (int)strlen(xml_data),
                                  "nc.xml", NULL,
                                  XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return xstrdup("");

    strbuf_t sb = {NULL, 0, 0};

    /* Tìm <data> node */
    xmlNodePtr data_node = NULL;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root) {
        for (xmlNodePtr n = root->children; n; n = n->next) {
            if (n->type == XML_ELEMENT_NODE &&
                strcmp((char *)n->name, "data") == 0) {
                data_node = n;
                break;
            }
        }
    }
    if (!data_node) data_node = root;

    if (data_node) {
        for (xmlNodePtr c = data_node->children; c; c = c->next) {
            render_node(&sb, c, 0, false,
                        path, path_len, 0);
        }
    }

    xmlFreeDoc(doc);
    if (!sb.buf) return xstrdup("");
    return sb.buf; /* caller free */
}

/* -------------------------------------------------------------------------
 * fmt_text_to_xml — text config format → NETCONF XML
 * Ví dụ input:
 *   system
 *     hostname new-name
 *
 * Output:
 *   <system><hostname>new-name</hostname></system>
 * ---------------------------------------------------------------------- */
char *fmt_text_to_xml(const char *text, schema_node_t *schema) {
    (void)schema; /* dùng cho lookup namespace nếu cần */
    if (!text) return NULL;

    strbuf_t sb  = {NULL, 0, 0};
    char   **tags = calloc(MAX_PATH_DEPTH, sizeof(char *));
    int      depth = 0;

    char *dup  = xstrdup(text);
    char *line = strtok(dup, "\n");

    while (line) {
        /* Đếm indent (2 spaces per level) */
        int spaces = 0;
        while (line[spaces] == ' ') spaces++;
        int level = spaces / 2;

        /* Trim */
        char *content = line + spaces;
        while (*content == '\t') content++;
        if (*content == '\0' || *content == '#') {
            line = strtok(NULL, "\n");
            continue;
        }

        /* Đóng tags thừa */
        while (depth > level) {
            depth--;
            sb_printf(&sb, "</%s>", tags[depth]);
            free(tags[depth]);
            tags[depth] = NULL;
        }

        /* Parse "key value" */
        char *space = strchr(content, ' ');
        if (space) {
            *space = '\0';
            char *val = space + 1;
            while (*val == ' ') val++;
            /* Leaf với value */
            sb_printf(&sb, "<%s>%s</%s>", content, val, content);
        } else {
            /* Container hoặc list */
            sb_printf(&sb, "<%s>", content);
            tags[depth] = xstrdup(content);
            depth++;
        }
        line = strtok(NULL, "\n");
    }

    /* Đóng tags còn lại */
    while (depth > 0) {
        depth--;
        sb_printf(&sb, "</%s>", tags[depth]);
        free(tags[depth]);
    }

    free(tags);
    free(dup);
    return sb.buf;
}
