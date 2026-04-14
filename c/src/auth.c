/*
 * auth.c — HTTP client gọi mgt-service REST API (libcurl + cJSON)
 *
 * Endpoints:
 *   POST /aa/authenticate          → JWT token
 *   GET  /aa/list/ne               → danh sách NE
 *   GET  /aa/list/ne/config        → NETCONF credentials mỗi NE
 *   POST /aa/history/save          → lưu command history
 *   POST /aa/backup/save           → lưu config snapshot
 *   GET  /aa/backup/list?ne=<name> → danh sách backup
 *   GET  /aa/backup/<id>           → lấy XML của một backup
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "cli.h"

/* -------------------------------------------------------------------------
 * libcurl write callback — tích luỹ response vào buffer
 * ---------------------------------------------------------------------- */
typedef struct {
    char  *data;
    size_t size;
} curl_buf_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    curl_buf_t *buf = userdata;
    size_t total    = size * nmemb;
    char  *tmp      = realloc(buf->data, buf->size + total + 1);
    if (!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->size, ptr, total);
    buf->size        += total;
    buf->data[buf->size] = '\0';
    return total;
}

/* -------------------------------------------------------------------------
 * Helpers: JSON extraction không cần external JSON library
 * Chỉ dùng cho các trường hợp đơn giản (string scalar fields)
 * ---------------------------------------------------------------------- */

/* Lấy giá trị string của key trong JSON object đơn giản */
static char *json_get_str(const char *json, const char *key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    /* bỏ qua whitespace và ':' */
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        const char *end = strchr(p, '"');
        if (!end) return NULL;
        size_t len = (size_t)(end - p);
        char *val = malloc(len + 1);
        if (!val) return NULL;
        memcpy(val, p, len);
        val[len] = '\0';
        return val;
    }
    /* số */
    const char *end = p;
    while (*end && *end != ',' && *end != '}' && *end != '\n') end++;
    size_t len = (size_t)(end - p);
    char *val = malloc(len + 1);
    if (!val) return NULL;
    memcpy(val, p, len);
    val[len] = '\0';
    /* trim trailing whitespace */
    while (len > 0 && (val[len-1] == ' ' || val[len-1] == '\n')) {
        val[--len] = '\0';
    }
    return val;
}

/* Thực hiện HTTP request, trả về body (caller free) */
static char *http_request(cli_session_t *s, const char *method,
                           const char *path, const char *body,
                           long *http_code) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    char url[MAX_URL_LEN * 2];
    snprintf(url, sizeof(url), "%s%s", s->mgt_url, path);

    curl_buf_t resp = {NULL, 0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    /* Authorization header nếu có token */
    if (s->token[0]) {
        char auth[MAX_TOKEN_LEN + 32];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", s->token);
        headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    } else if (strcmp(method, "GET") == 0) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    CURLcode rc = curl_easy_perform(curl);
    if (http_code)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(resp.data);
        return NULL;
    }
    return resp.data; /* caller free */
}

/* -------------------------------------------------------------------------
 * http_fire_forget — POST fire-and-forget, timeout 3s, log lỗi, không crash
 * Dùng cho history/backup — không cần biết kết quả.
 * ---------------------------------------------------------------------- */
static void http_fire_forget(cli_session_t *s, const char *path,
                             const char *body) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "[mgt] curl_easy_init failed, skip %s\n", path);
        return;
    }

    char url[MAX_URL_LEN * 2];
    snprintf(url, sizeof(url), "%s%s", s->mgt_url, path);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (s->token[0]) {
        char auth[MAX_TOKEN_LEN + 32];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", s->token);
        headers = curl_slist_append(headers, auth);
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb); /* discard body */
    curl_buf_t discard = {NULL, 0};
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &discard);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 2L);  /* fail fast */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "{}");

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fprintf(stderr, "[mgt] %s failed: %s (skipped)\n",
                path, curl_easy_strerror(rc));
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (code != 200 && code != 201 && code != 204) {
            fprintf(stderr, "[mgt] %s returned HTTP %ld (skipped)\n",
                    path, code);
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(discard.data);
}

/* -------------------------------------------------------------------------
 * auth_login — POST /aa/authenticate
 * ---------------------------------------------------------------------- */
bool auth_login(cli_session_t *s, const char *username, const char *password) {
    char body[512];
    snprintf(body, sizeof(body),
             "{\"username\":\"%s\",\"password\":\"%s\"}", username, password);

    long code = 0;
    char *resp = http_request(s, "POST", "/aa/authenticate", body, &code);
    if (!resp || code != 200) { free(resp); return false; }

    /* Mock returns "response_data", real service may use "token" */
    char *token = json_get_str(resp, "response_data");
    if (!token) token = json_get_str(resp, "token");
    free(resp);
    if (!token) return false;

    strncpy(s->token, token, sizeof(s->token) - 1);
    strncpy(s->username, username, sizeof(s->username) - 1);
    free(token);
    return true;
}

/* -------------------------------------------------------------------------
 * auth_list_ne — GET /aa/list/ne
 * Parse JSON array để điền s->nes[]
 * ---------------------------------------------------------------------- */
bool auth_list_ne(cli_session_t *s) {
    long code = 0;
    char *resp = http_request(s, "GET", "/aa/list/ne", NULL, &code);
    /* Mock returns 302; real service may return 200 */
    if (!resp || (code != 200 && code != 302)) { free(resp); return false; }

    s->ne_count = 0;

    /* Parse mảng JSON neDataList: [...] (mock key) or ne_data_list */
    const char *arr = strstr(resp, "neDataList");
    if (!arr) arr = strstr(resp, "ne_data_list");
    if (!arr) arr = resp;

    /* Tìm từng object { ... } */
    const char *p = arr;
    while (s->ne_count < MAX_NE) {
        p = strchr(p, '{');
        if (!p) break;

        /* Extract từng object đến } matching */
        int depth = 1;
        const char *obj_start = p + 1;
        const char *q = p + 1;
        while (*q && depth > 0) {
            if (*q == '{') depth++;
            else if (*q == '}') depth--;
            q++;
        }
        /* [p+1 .. q-2] là body của object */
        size_t obj_len = (size_t)(q - p - 1);
        char *obj = malloc(obj_len + 1);
        if (!obj) break;
        memcpy(obj, obj_start, obj_len - 1);
        obj[obj_len - 1] = '\0';

        ne_info_t *ne = &s->nes[s->ne_count];
        memset(ne, 0, sizeof(*ne));

        char *v;
        if ((v = json_get_str(obj, "ne"))) {
            strncpy(ne->name, v, sizeof(ne->name)-1); free(v);
        }
        if ((v = json_get_str(obj, "site"))) {
            strncpy(ne->site, v, sizeof(ne->site)-1); free(v);
        }
        if ((v = json_get_str(obj, "ip"))) {
            strncpy(ne->ip, v, sizeof(ne->ip)-1); free(v);
        }
        if ((v = json_get_str(obj, "port"))) {
            ne->port = atoi(v); free(v);
        }
        if ((v = json_get_str(obj, "namespace"))) {
            strncpy(ne->ns, v, sizeof(ne->ns)-1); free(v);
        }
        if ((v = json_get_str(obj, "description"))) {
            strncpy(ne->description, v, sizeof(ne->description)-1); free(v);
        }
        if (ne->name[0]) s->ne_count++;
        free(obj);
        p = q;
    }

    free(resp);
    return s->ne_count > 0;
}

/* -------------------------------------------------------------------------
 * auth_save_history — POST /aa/history/save  (fire-and-forget)
 * ---------------------------------------------------------------------- */
void auth_save_history(cli_session_t *s, const char *cmd, long ms) {
    if (!s->token[0] || !s->current_ne) return;  /* direct mode → skip */

    char body[MAX_LINE_LEN];
    snprintf(body, sizeof(body),
        "{\"cmd_name\":\"%s\",\"ne_name\":\"%s\",\"ne_ip\":\"%s\","
        "\"input_type\":\"cli\",\"result\":\"success\","
        "\"time_to_complete\":%ld}",
        cmd, s->current_ne->name, s->current_ne->ip, ms);

    http_fire_forget(s, "/aa/history/save", body);
}

/* -------------------------------------------------------------------------
 * auth_save_backup — POST /aa/backup/save
 * ---------------------------------------------------------------------- */
bool auth_save_backup(cli_session_t *s, const char *config_xml,
                      int *out_remote_id) {
    if (!s->token[0] || !s->current_ne || !config_xml) return false;

    /* Escape double quotes trong XML để embed vào JSON */
    size_t xml_len = strlen(config_xml);
    char  *escaped = malloc(xml_len * 2 + 1);
    if (!escaped) return false;

    size_t j = 0;
    for (size_t i = 0; i < xml_len; i++) {
        if (config_xml[i] == '"')  { escaped[j++] = '\\'; escaped[j++] = '"'; }
        else if (config_xml[i] == '\n') { escaped[j++] = '\\'; escaped[j++] = 'n'; }
        else if (config_xml[i] == '\r') { /* skip */ }
        else escaped[j++] = config_xml[i];
    }
    escaped[j] = '\0';

    size_t body_len = xml_len * 2 + 256;
    char *body = malloc(body_len);
    if (!body) { free(escaped); return false; }
    snprintf(body, body_len,
        "{\"ne_name\":\"%s\",\"ne_ip\":\"%s\",\"config_xml\":\"%s\"}",
        s->current_ne->name, s->current_ne->ip, escaped);
    free(escaped);

    long code = 0;
    char *resp = http_request(s, "POST", "/aa/backup/save", body, &code);
    free(body);

    if (!resp || (code != 200 && code != 201)) {
        fprintf(stderr, "[mgt] backup/save failed (HTTP %ld) — skipped\n", code);
        free(resp);
        return false;
    }

    char *id_str = json_get_str(resp, "id");
    free(resp);
    if (id_str) {
        if (out_remote_id) *out_remote_id = atoi(id_str);
        free(id_str);
        return true;
    }
    return false;
}

/* -------------------------------------------------------------------------
 * auth_list_backups — GET /aa/backup/list?ne=<name>
 * Điền s->backups[], s->backup_count
 * ---------------------------------------------------------------------- */
bool auth_list_backups(cli_session_t *s) {
    if (!s->token[0] || !s->current_ne) return false;

    char path[256];
    snprintf(path, sizeof(path), "/aa/backup/list?ne=%s", s->current_ne->name);

    long code = 0;
    char *resp = http_request(s, "GET", path, NULL, &code);
    if (!resp || code != 200) { free(resp); return false; }

    /* Free old backups */
    if (s->backups) {
        for (int i = 0; i < s->backup_count; i++) free(s->backups[i].xml);
        free(s->backups);
        s->backups = NULL;
        s->backup_count = 0;
    }

    /* Count objects */
    int count = 0;
    const char *p = resp;
    while ((p = strchr(p, '{')) != NULL) { count++; p++; }
    if (count == 0) { free(resp); return true; }

    s->backups = calloc(count, sizeof(backup_t));
    if (!s->backups) { free(resp); return false; }

    p = resp;
    int idx = 0;
    while (idx < count) {
        p = strchr(p, '{');
        if (!p) break;
        int depth = 1;
        const char *start = p + 1;
        const char *q = p + 1;
        while (*q && depth > 0) {
            if (*q == '{') depth++;
            else if (*q == '}') depth--;
            q++;
        }
        size_t len = (size_t)(q - p - 1);
        char *obj = malloc(len);
        if (!obj) break;
        memcpy(obj, start, len - 1);
        obj[len - 1] = '\0';

        backup_t *b = &s->backups[idx];
        b->id = idx + 1;
        char *v;
        if ((v = json_get_str(obj, "id"))) { b->remote_id = atoi(v); free(v); }
        if ((v = json_get_str(obj, "created_at"))) {
            strncpy(b->timestamp, v, sizeof(b->timestamp)-1); free(v);
        }
        b->xml = NULL; /* lazy */
        idx++;
        free(obj);
        p = q;
    }
    s->backup_count = idx;
    s->backup_seq   = idx;
    free(resp);
    return true;
}

/* -------------------------------------------------------------------------
 * auth_get_backup — GET /aa/backup/<id>
 * ---------------------------------------------------------------------- */
bool auth_get_backup(cli_session_t *s, int remote_id, char **out_xml) {
    if (!s->token[0]) return false;

    char path[128];
    snprintf(path, sizeof(path), "/aa/backup/%d", remote_id);

    long code = 0;
    char *resp = http_request(s, "GET", path, NULL, &code);
    if (!resp || code != 200) { free(resp); return false; }

    char *xml = json_get_str(resp, "config_xml");
    free(resp);
    if (!xml) return false;
    *out_xml = xml;
    return true;
}
