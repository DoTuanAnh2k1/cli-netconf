/*
 * schema.c — Schema tree cho tab completion
 *
 * Build từ 2 nguồn (ưu tiên theo thứ tự):
 *   1. MAAPI (libconfd) — khi compile WITH_MAAPI và ConfD chạy locally
 *   2. NETCONF get-schema (RFC 6022) — parse YANG text
 *   3. Fallback: parse XML running-config
 *
 * Schema tree là linked list đơn giản, đủ cho tab completion.
 * Không cần đầy đủ YANG semantics — chỉ cần biết container/list nào tồn tại.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "cli.h"

/* -------------------------------------------------------------------------
 * Schema node management
 * ---------------------------------------------------------------------- */
schema_node_t *schema_new_node(const char *name) {
    schema_node_t *n = calloc(1, sizeof(*n));
    if (!n) return NULL;
    strncpy(n->name, name, MAX_NAME_LEN - 1);
    return n;
}

void schema_free(schema_node_t *root) {
    if (!root) return;
    schema_free(root->children);
    schema_free(root->next);
    free(root);
}

/* Tìm hoặc tạo child có tên `name` trong parent */
static schema_node_t *find_or_create_child(schema_node_t *parent,
                                            const char *name) {
    for (schema_node_t *c = parent->children; c; c = c->next) {
        if (strcmp(c->name, name) == 0) return c;
    }
    schema_node_t *n = schema_new_node(name);
    if (!n) return NULL;
    n->next = parent->children;
    parent->children = n;
    return n;
}

/* -------------------------------------------------------------------------
 * schema_lookup — tìm node theo path (mảng string)
 * ---------------------------------------------------------------------- */
schema_node_t *schema_lookup(schema_node_t *root,
                              const char **path, int depth) {
    schema_node_t *node = root;
    for (int i = 0; i < depth; i++) {
        if (!path[i] || path[i][0] == '\0') continue;
        schema_node_t *child = NULL;
        for (schema_node_t *c = node->children; c; c = c->next) {
            if (strcmp(c->name, path[i]) == 0) { child = c; break; }
        }
        if (!child) return NULL;
        node = child;
    }
    return node;
}

/* -------------------------------------------------------------------------
 * schema_merge — merge src vào dst (dst ưu tiên)
 * ---------------------------------------------------------------------- */
void schema_merge(schema_node_t *dst, schema_node_t *src) {
    if (!dst || !src) return;
    for (schema_node_t *sc = src->children; sc; sc = sc->next) {
        schema_node_t *dc = NULL;
        for (schema_node_t *c = dst->children; c; c = c->next) {
            if (strcmp(c->name, sc->name) == 0) { dc = c; break; }
        }
        if (!dc) {
            /* Copy node (shallow) và append */
            dc = schema_new_node(sc->name);
            if (!dc) continue;
            dc->is_list = sc->is_list;
            dc->is_leaf = sc->is_leaf;
            strncpy(dc->ns, sc->ns, MAX_NS_LEN - 1);
            dc->next = dst->children;
            dst->children = dc;
        }
        schema_merge(dc, sc);
    }
}

/* -------------------------------------------------------------------------
 * schema_child_names — trả về mảng tên con (caller free[])
 * ---------------------------------------------------------------------- */
char **schema_child_names(schema_node_t *node, int *count) {
    *count = 0;
    if (!node) return NULL;

    int n = 0;
    for (schema_node_t *c = node->children; c; c = c->next) n++;
    if (n == 0) return NULL;

    char **names = calloc(n + 1, sizeof(char *));
    if (!names) return NULL;

    int i = 0;
    for (schema_node_t *c = node->children; c; c = c->next) {
        names[i++] = xstrdup(c->name);
    }
    *count = n;
    return names;
}

/* -------------------------------------------------------------------------
 * schema_parse_xml — build schema từ XML get-config response (libxml2)
 * ---------------------------------------------------------------------- */
static void build_from_xml(xmlNodePtr xml_parent, schema_node_t *schema_parent,
                            schema_node_t *root) {
    for (xmlNodePtr n = xml_parent->children; n; n = n->next) {
        if (n->type != XML_ELEMENT_NODE) continue;
        const char *name = (char *)n->name;
        schema_node_t *child = find_or_create_child(schema_parent, name);
        if (!child) continue;
        /* Capture namespace từ top-level containers */
        if (schema_parent == root && n->ns && n->ns->href) {
            strncpy(child->ns, (char *)n->ns->href, MAX_NS_LEN - 1);
        }
        /* Nếu không có element con → là leaf */
        bool has_elem_child = false;
        for (xmlNodePtr c = n->children; c; c = c->next) {
            if (c->type == XML_ELEMENT_NODE) { has_elem_child = true; break; }
        }
        child->is_leaf = !has_elem_child;
        build_from_xml(n, child, root);
    }
}

schema_node_t *schema_parse_xml(const char *xml_data) {
    if (!xml_data) return NULL;

    xmlDocPtr doc = xmlReadMemory(xml_data, (int)strlen(xml_data),
                                  "nc.xml", NULL, XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return NULL;

    schema_node_t *root = schema_new_node("__root__");
    if (!root) { xmlFreeDoc(doc); return NULL; }

    /* Tìm <data> node trong rpc-reply */
    xmlNodePtr data_node = NULL;
    xmlNodePtr cur = xmlDocGetRootElement(doc);
    if (cur) {
        /* rpc-reply → data */
        for (xmlNodePtr n = cur->children; n; n = n->next) {
            if (n->type == XML_ELEMENT_NODE &&
                strcmp((char *)n->name, "data") == 0) {
                data_node = n;
                break;
            }
        }
    }
    if (!data_node) data_node = cur;

    if (data_node) build_from_xml(data_node, root, root);
    xmlFreeDoc(doc);
    return root;
}

/* -------------------------------------------------------------------------
 * YANG text parser — 2-pass (grouping expand + tree build)
 * Ported từ Go completer.go với cùng logic
 * ---------------------------------------------------------------------- */

/* Grouping map entry */
typedef struct grouping_entry {
    char              name[MAX_NAME_LEN];
    schema_node_t    *node;   /* subtree của grouping */
    struct grouping_entry *next;
} grouping_entry_t;

static grouping_entry_t *find_grouping(grouping_entry_t *map, const char *name) {
    for (grouping_entry_t *e = map; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e;
    }
    return NULL;
}

static void free_groupings(grouping_entry_t *map) {
    while (map) {
        grouping_entry_t *next = map->next;
        schema_free(map->node);
        free(map);
        map = next;
    }
}

/* Strip YANG module prefix: "smf:vsmf-grp" → "vsmf-grp" */
static const char *strip_prefix(const char *name) {
    const char *p = strrchr(name, ':');
    return p ? p + 1 : name;
}

/* Kiểm tra keyword metadata YANG */
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

static int count_char(const char *s, char c) {
    int n = 0;
    while (*s) { if (*s == c) n++; s++; }
    return n;
}

/* Parse grouping body text thành schema node */
static schema_node_t *parse_grouping_body(const char *text,
                                          grouping_entry_t *groupings) {
    schema_node_t *root  = schema_new_node("__grp__");
    schema_node_t *stack[MAX_PATH_DEPTH];
    int            depth = 0;
    stack[depth++] = root;
    int meta_depth = 0;

    char *text_copy = xstrdup(text);
    char *line = strtok(text_copy, "\n");
    while (line) {
        /* Trim */
        while (*line == ' ' || *line == '\t') line++;
        char *end = line + strlen(line) - 1;
        while (end > line && (*end == ' ' || *end == '\t' || *end == '\r')) {
            *end-- = '\0';
        }

        if (*line == '\0') { line = strtok(NULL, "\n"); continue; }

        int opens  = count_char(line, '{');
        int closes = count_char(line, '}');

        if (meta_depth > 0) {
            meta_depth += opens - closes;
            line = strtok(NULL, "\n"); continue;
        }

        if (strcmp(line, "}") == 0) {
            if (depth > 1) depth--;
            line = strtok(NULL, "\n"); continue;
        }

        if (is_yang_meta(line)) {
            if (opens > closes) meta_depth += opens - closes;
            line = strtok(NULL, "\n"); continue;
        }

        if (strncmp(line, "uses ", 5) == 0) {
            char gname[MAX_NAME_LEN];
            strncpy(gname, line + 5, MAX_NAME_LEN - 1);
            /* strip trailing { or ; */
            char *p = strpbrk(gname, " {;");
            if (p) *p = '\0';
            const char *bare = strip_prefix(gname);
            grouping_entry_t *g = find_grouping(groupings, bare);
            if (g) schema_merge(stack[depth - 1], g->node);
            line = strtok(NULL, "\n"); continue;
        }

        static const char *kws[] = {"container ", "list ", "leaf-list ", "leaf ", NULL};
        for (int k = 0; kws[k]; k++) {
            if (strncmp(line, kws[k], strlen(kws[k])) == 0) {
                char name[MAX_NAME_LEN];
                const char *p = line + strlen(kws[k]);
                int ni = 0;
                while (*p && *p != ' ' && *p != '{' && *p != ';' && ni < MAX_NAME_LEN-1)
                    name[ni++] = *p++;
                name[ni] = '\0';

                schema_node_t *parent = stack[depth - 1];
                schema_node_t *child  = find_or_create_child(parent, name);
                if (child) {
                    child->is_list = (kws[k][0] == 'l' && kws[k][1] == 'i');
                    child->is_leaf = (strncmp(kws[k], "leaf", 4) == 0);

                    /* Só "{" ở cuối → push stack */
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
 * schema_parse_yang — parse YANG text, trả về schema root
 * ---------------------------------------------------------------------- */
schema_node_t *schema_parse_yang(const char *yang_text, const char *ns) {
    if (!yang_text) return NULL;

    /* Pass 1: collect groupings */
    grouping_entry_t *groupings = NULL;
    {
        char *text = xstrdup(yang_text);
        char *p    = text;
        char *line;
        while ((line = strsep(&p, "\n")) != NULL) {
            /* Trim */
            while (*line == ' ' || *line == '\t') line++;
            if (strncmp(line, "grouping ", 9) != 0) continue;

            char gname[MAX_NAME_LEN] = {0};
            const char *q = line + 9;
            int ni = 0;
            while (*q && *q != ' ' && *q != '{' && ni < MAX_NAME_LEN-1)
                gname[ni++] = *q++;
            gname[ni] = '\0';

            /* Collect body until matching } */
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
                if (dep <= 0) break;
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

    /* Pass 2: build schema tree, expand uses, skip grouping blocks */
    schema_node_t *root  = schema_new_node("__root__");
    schema_node_t *stack[MAX_PATH_DEPTH];
    int            depth = 0;
    stack[depth++] = root;

    int meta_depth    = 0;
    int grp_depth     = 0;
    bool in_grouping  = false;

    char *text = xstrdup(yang_text);
    char *p    = text;
    char *line;
    while ((line = strsep(&p, "\n")) != NULL) {
        while (*line == ' ' || *line == '\t') line++;
        char *end = line + strlen(line) - 1;
        while (end > line && (*end == ' ' || *end == '\t' || *end == '\r')) *end-- = '\0';
        if (*line == '\0') continue;

        int opens  = count_char(line, '{');
        int closes = count_char(line, '}');

        if (in_grouping) {
            grp_depth += opens - closes;
            if (grp_depth <= 0) in_grouping = false;
            continue;
        }
        if (meta_depth > 0) {
            meta_depth += opens - closes;
            continue;
        }
        if (strncmp(line, "module ", 7) == 0) {
            if (line[strlen(line)-1] == '{') stack[depth > 0 ? depth-1 : 0] = root;
            continue;
        }
        if (strncmp(line, "grouping ", 9) == 0) {
            in_grouping = true;
            grp_depth   = opens - closes;
            continue;
        }
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
        if (strncmp(line, "augment ", 8) == 0) {
            /* Parse path và navigate/create */
            char raw[MAX_LINE_LEN];
            strncpy(raw, line + 8, MAX_LINE_LEN - 1);
            char *ep = strpbrk(raw, " {");
            if (ep) *ep = '\0';
            /* Strip quotes */
            char *path_str = raw;
            while (*path_str == '"' || *path_str == '\'') path_str++;
            char *path_end = path_str + strlen(path_str) - 1;
            while (path_end > path_str && (*path_end == '"' || *path_end == '\''))
                *path_end-- = '\0';

            schema_node_t *target = root;
            char *seg = strtok(path_str, "/");
            while (seg) {
                const char *bare_seg = strip_prefix(seg);
                if (*bare_seg) target = find_or_create_child(target, bare_seg);
                seg = strtok(NULL, "/");
            }
            if (line[strlen(line)-1] == '{' && depth < MAX_PATH_DEPTH)
                stack[depth++] = target;
            continue;
        }
        if (strcmp(line, "}") == 0) {
            if (depth > 1) depth--;
            continue;
        }
        if (is_yang_meta(line)) {
            if (opens > closes) meta_depth += opens - closes;
            continue;
        }

        static const char *kws[] = {"container ", "list ", "leaf-list ", "leaf ", NULL};
        for (int k = 0; kws[k]; k++) {
            if (strncmp(line, kws[k], strlen(kws[k])) == 0) {
                char name[MAX_NAME_LEN];
                const char *q = line + strlen(kws[k]);
                int ni = 0;
                while (*q && *q != ' ' && *q != '{' && *q != ';' && ni < MAX_NAME_LEN-1)
                    name[ni++] = *q++;
                name[ni] = '\0';

                schema_node_t *parent = stack[depth - 1];
                schema_node_t *child  = find_or_create_child(parent, name);
                if (child) {
                    child->is_list = (kws[k][0] == 'l' && kws[k][1] == 'i');
                    child->is_leaf = (strncmp(kws[k], "leaf", 4) == 0);
                    /* Gắn namespace cho top-level containers */
                    if (depth == 1 && ns && ns[0] && !child->ns[0])
                        strncpy(child->ns, ns, MAX_NS_LEN - 1);
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
 * Helpers cho schema_load
 * ---------------------------------------------------------------------- */

/* Lấy YANG text từ get-schema rpc-reply */
static char *extract_schema_text(const char *reply) {
    const char *start = strstr(reply, "<data");
    if (!start) return NULL;
    const char *gt = strchr(start, '>');
    if (!gt) return NULL;
    const char *content = gt + 1;
    const char *end     = strstr(reply, "</data>");
    if (!end || end <= content) return NULL;

    size_t len = (size_t)(end - content);
    char *text = malloc(len + 1);
    if (!text) return NULL;
    memcpy(text, content, len);
    text[len] = '\0';

    /* Unescape XML entities */
    char *out = malloc(len + 1);
    if (!out) { free(text); return NULL; }
    size_t i = 0, j = 0;
    while (i < len) {
        if (text[i] == '&') {
            if (strncmp(text+i, "&amp;",  5) == 0) { out[j++]='&';  i+=5; }
            else if (strncmp(text+i, "&lt;",   4) == 0) { out[j++]='<';  i+=4; }
            else if (strncmp(text+i, "&gt;",   4) == 0) { out[j++]='>';  i+=4; }
            else if (strncmp(text+i, "&quot;", 6) == 0) { out[j++]='"';  i+=6; }
            else out[j++] = text[i++];
        } else out[j++] = text[i++];
    }
    out[j] = '\0';
    free(text);
    return out;
}

/* Namespace → candidates tên module (tương tự Go moduleNamesFromNamespace) */
static char **ns_to_module_candidates(const char *ns, int *count) {
    *count = 0;
    if (!ns || !ns[0]) return NULL;

    /* Split by '/' và ':' */
    char *dup = xstrdup(ns);
    char *segs[64];
    int   seg_count = 0;
    char *p = dup;
    char *tok;
    while ((tok = strsep(&p, "/:")) && seg_count < 64) {
        if (tok[0]) segs[seg_count++] = tok;
    }

    static const char *noise[] = {
        "http","https","urn","www","yang","ietf","params","xml","ns",NULL
    };
    char **cands   = calloc(32, sizeof(char *));
    bool  seen[64] = {false};
    int   n        = 0;

    for (int i = seg_count - 1; i >= 0 && n < 16; i--) {
        char *seg = segs[i];
        /* Bỏ qua noise */
        bool is_noise = false;
        for (int ni = 0; noise[ni]; ni++) {
            if (strcasecmp(seg, noise[ni]) == 0) { is_noise = true; break; }
        }
        if (is_noise) continue;
        /* Bỏ qua version-like (bắt đầu bằng số) */
        if (isdigit((unsigned char)seg[0])) continue;
        /* Tránh trùng */
        bool dup_seg = false;
        for (int j = 0; j < n; j++) {
            if (strcasecmp(cands[j], seg) == 0) { dup_seg = true; break; }
        }
        if (dup_seg) continue;

        cands[n++] = xstrdup(seg);
        (void)seen;

        /* Thêm biến thể dash/underscore */
        if (strchr(seg, '-') && n < 28) {
            char *alt = xstrdup(seg);
            for (char *c = alt; *c; c++) if (*c == '-') *c = '_';
            cands[n++] = alt;
        } else if (strchr(seg, '_') && n < 28) {
            char *alt = xstrdup(seg);
            for (char *c = alt; *c; c++) if (*c == '_') *c = '-';
            cands[n++] = alt;
        }
    }
    free(dup);
    *count = n;
    return cands;
}

/* -------------------------------------------------------------------------
 * schema_load — gọi từ session sau khi kết nối NE
 * ---------------------------------------------------------------------- */
void schema_load(cli_session_t *s) {
    schema_free(s->schema);
    s->schema = schema_new_node("__root__");
    if (!s->schema) return;

#ifdef WITH_MAAPI
    /* Ưu tiên MAAPI khi có libconfd */
    if (maapi_load_schema(s)) {
        fprintf(stderr, "[schema] loaded via MAAPI\n");
        return;
    }
#endif

    /* Phase 1: NETCONF get-schema cho modules trong capabilities */
    int mod_count = 0;
    char **modules = nc_extract_modules(s->nc, &mod_count);
    for (int i = 0; i < mod_count; i++) {
        char *reply = nc_get_schema(s->nc, modules[i]);
        if (!reply) { free(modules[i]); continue; }

        char *yang = extract_schema_text(reply);
        free(reply);
        if (!yang) { free(modules[i]); continue; }

        /* Tìm namespace từ capabilities */
        char ns[MAX_NS_LEN] = {0};
        for (int j = 0; j < s->nc->cap_count; j++) {
            char search[MAX_NAME_LEN + 16];
            snprintf(search, sizeof(search), "module=%s", modules[i]);
            if (strstr(s->nc->capabilities[j], search)) {
                /* URI trước '?' */
                const char *q = strchr(s->nc->capabilities[j], '?');
                size_t len = q ? (size_t)(q - s->nc->capabilities[j])
                               : strlen(s->nc->capabilities[j]);
                if (len < MAX_NS_LEN) {
                    memcpy(ns, s->nc->capabilities[j], len);
                    ns[len] = '\0';
                }
                break;
            }
        }

        schema_node_t *parsed = schema_parse_yang(yang, ns);
        free(yang);
        if (parsed) {
            schema_merge(s->schema, parsed);
            schema_free(parsed);
        }
        free(modules[i]);
    }
    free(modules);

    /* Phase 2: running-config XML để capture nodes không có trong YANG */
    char *reply = nc_get_config(s->nc, "running", "");
    if (!reply) return;

    schema_node_t *xml_schema = schema_parse_xml(reply);
    free(reply);
    if (!xml_schema) return;

    for (schema_node_t *xc = xml_schema->children; xc; xc = xc->next) {
        /* Nếu đã có trong YANG → skip */
        bool in_yang = false;
        for (schema_node_t *yc = s->schema->children; yc; yc = yc->next) {
            if (strcmp(yc->name, xc->name) == 0) { in_yang = true; break; }
        }
        if (in_yang) continue;

        /* Thử get-schema qua namespace */
        int cand_count = 0;
        char **cands = ns_to_module_candidates(xc->ns, &cand_count);
        bool loaded  = false;

        for (int i = 0; i < cand_count && !loaded; i++) {
            char *yang_reply = nc_get_schema(s->nc, cands[i]);
            if (!yang_reply) { free(cands[i]); continue; }

            char *yang = extract_schema_text(yang_reply);
            free(yang_reply);
            if (!yang) { free(cands[i]); continue; }

            schema_node_t *parsed = schema_parse_yang(yang, xc->ns);
            free(yang);
            if (parsed) {
                schema_merge(s->schema, parsed);
                schema_free(parsed);
                loaded = true;
            }
            free(cands[i]);
        }
        for (int i = loaded ? cand_count : 0; i < cand_count; i++) free(cands[i]);
        free(cands);

        /* Fallback: dùng XML node */
        if (!loaded) {
            schema_node_t *copy = schema_new_node(xc->name);
            if (copy) {
                strncpy(copy->ns, xc->ns, MAX_NS_LEN - 1);
                schema_merge(copy, xc);
                copy->next = s->schema->children;
                s->schema->children = copy;
            }
        }
    }
    schema_free(xml_schema);
}

/* -------------------------------------------------------------------------
 * Utility: xstrdup
 * ---------------------------------------------------------------------- */
char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char  *dup = malloc(len + 1);
    if (dup) memcpy(dup, s, len + 1);
    return dup;
}

char **str_split(const char *s, int *count) {
    *count = 0;
    if (!s) return NULL;
    char *dup = xstrdup(s);
    /* Đếm token */
    int n = 0;
    char *p = dup;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        n++;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (n == 0) { free(dup); return NULL; }

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

void free_tokens(char **tokens, int count) {
    if (!tokens) return;
    for (int i = 0; i < count; i++) free(tokens[i]);
    free(tokens);
}

char *str_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
        *end-- = '\0';
    return s;
}
