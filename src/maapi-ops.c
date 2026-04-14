/*
 * maapi-ops.c — Các thao tác cấu hình dựa trên MAAPI (Management Agent API)
 *
 * File này triển khai các thao tác CRUD trên cấu hình ConfD thông qua MAAPI,
 * tương đương với các NETCONF operation nhưng sử dụng IPC trực tiếp:
 *
 *   get-config  → maapi_save_config (MAAPI_CONFIG_XML)
 *   edit-config → maapi_load_config (MAAPI_CONFIG_XML | MAAPI_CONFIG_MERGE)
 *   set leaf    → maapi_set_elem / maapi_set_elem2
 *   delete      → maapi_delete
 *   commit      → maapi_candidate_commit
 *   discard     → maapi_candidate_reset
 *   validate    → maapi_validate_trans
 *
 * Ưu điểm so với NETCONF: không cần SSH, không cần XML envelope,
 * hiệu năng tốt hơn cho các thao tác nội bộ trên cùng máy chủ.
 */
#ifdef WITH_MAAPI

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "confd_compat.h"
#include "cli.h"
#include "maapi-direct.h"

/* ─── Hàm trợ giúp nội bộ (internal helpers) ──────────────── */

/*
 * read_file_str — Đọc toàn bộ nội dung file vào chuỗi ký tự
 *
 * Tham số:
 *   path — đường dẫn tuyệt đối tới file cần đọc
 *
 * Trả về:
 *   Con trỏ tới chuỗi chứa nội dung file (malloc'd, caller phải free),
 *   hoặc NULL nếu không mở được file hoặc không đủ bộ nhớ.
 *   Trả về chuỗi rỗng (strdup("")) nếu file rỗng.
 */
static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    /* Xác định kích thước file bằng cách seek tới cuối */
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    /* File rỗng → trả về chuỗi rỗng */
    if (sz <= 0) { fclose(f); return strdup(""); }

    /* Cấp phát bộ nhớ đủ chứa nội dung + ký tự null kết thúc */
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    /* Đọc toàn bộ nội dung file vào buffer */
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/*
 * write_tmp_str — Tạo file tạm và ghi chuỗi vào đó
 *
 * Dùng mkstemp() để tạo file tạm an toàn (tránh race condition).
 *
 * Tham số:
 *   str — chuỗi nội dung cần ghi vào file tạm (có thể NULL hoặc rỗng)
 *
 * Trả về:
 *   Đường dẫn file tạm (malloc'd, caller phải unlink rồi free),
 *   hoặc NULL nếu thất bại.
 */
static char *write_tmp_str(const char *str) {
    char *path = strdup("/tmp/maapi-XXXXXX");
    if (!path) return NULL;

    /* mkstemp tạo file tạm với tên duy nhất và mở nó */
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }

    /* Ghi nội dung chuỗi vào file (nếu có) */
    if (str && *str) {
        size_t len = strlen(str);
        ssize_t written = write(fd, str, len);
        (void)written; /* Bỏ qua giá trị trả về (best-effort write) */
    }
    close(fd);
    return path;
}

/*
 * ensure_write_trans — Đảm bảo đã mở write transaction trên candidate datastore
 *
 * Nếu chưa có write transaction đang mở, hàm này sẽ tạo mới.
 * Write transaction được dùng để tích lũy các thay đổi trước khi commit.
 *
 * Tham số:
 *   m — con trỏ tới phiên MAAPI hiện tại
 *
 * Trả về:
 *   0 nếu thành công (đã có hoặc vừa tạo xong), -1 nếu thất bại.
 */
static int ensure_write_trans(maapi_session_t *m) {
    if (m->has_write) return 0; /* Đã có write transaction → không cần làm gì */

    /* Mở write transaction mới trên candidate datastore */
    int th = maapi_start_trans(m->sock, CONFD_CANDIDATE, CONFD_READ_WRITE);
    if (th < 0) {
        fprintf(stderr, "[maapi] start_trans(write) failed: %s\n",
                confd_lasterr());
        return -1;
    }
    m->th_write  = th;
    m->has_write = true;
    return 0;
}

/*
 * drop_write_trans — Đóng write transaction hiện tại mà không commit
 *
 * Dùng sau thao tác discard để huỷ bỏ mọi thay đổi chưa commit.
 * Hàm maapi_finish_trans() đóng transaction phía ConfD server.
 *
 * Tham số:
 *   m — con trỏ tới phiên MAAPI hiện tại
 */
static void drop_write_trans(maapi_session_t *m) {
    if (!m->has_write) return;
    maapi_finish_trans(m->sock, m->th_write);
    m->has_write = false;
}

/* ─── Vòng đời phiên làm việc (Lifecycle) ─────────────────── */

/*
 * maapi_dial — Thiết lập kết nối MAAPI tới ConfD server
 *
 * Quy trình:
 *   1. Khởi tạo thư viện libconfd (confd_init)
 *   2. Tạo TCP socket và kết nối tới ConfD IPC port
 *   3. Mở user session (xác thực với ConfD)
 *   4. Trả về cấu trúc maapi_session_t chứa thông tin phiên
 *
 * Tham số:
 *   host — địa chỉ IP của ConfD (ví dụ: "127.0.0.1")
 *   port — cổng IPC của ConfD (mặc định 4565)
 *   user — tên người dùng (NULL → mặc định "admin")
 *
 * Trả về:
 *   Con trỏ tới maapi_session_t mới (caller phải gọi cli_session_close),
 *   hoặc NULL nếu kết nối thất bại.
 */
maapi_session_t *maapi_dial(const char *host, int port, const char *user) {
    /* Khởi tạo libconfd ở chế độ im lặng (không in debug) */
    confd_init("cli-netconf-maapi", stderr, CONFD_SILENT);

    /* Chuẩn bị địa chỉ TCP socket */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_aton(host, &addr.sin_addr) == 0) {
        fprintf(stderr, "[maapi] invalid address: %s\n", host);
        return NULL;
    }

    /* Tạo TCP socket */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    /* Kết nối tới ConfD qua giao thức MAAPI IPC */
    if (maapi_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != CONFD_OK) {
        fprintf(stderr, "[maapi] connect to %s:%d failed: %s\n",
                host, port, confd_lasterr());
        close(sock);
        return NULL;
    }

    /* Mở phiên người dùng — ConfD sẽ kiểm tra quyền truy cập */
    if (maapi_start_user_session(sock,
                                 user ? user : "admin",
                                 "system",     /* context: dùng cho audit log */
                                 NULL, 0,      /* groups: NULL, count: 0 */
                                 CONFD_PROTO_TCP) != CONFD_OK) {
        fprintf(stderr, "[maapi] start_user_session failed: %s\n",
                confd_lasterr());
        maapi_close(sock);
        return NULL;
    }

    /* Cấp phát và khởi tạo cấu trúc phiên MAAPI */
    maapi_session_t *m = calloc(1, sizeof(*m));
    if (!m) { close(sock); return NULL; }
    m->sock = sock;
    m->port = port;
    strncpy(m->host, host, sizeof(m->host) - 1);
    return m;
}

/*
 * cli_session_close — Đóng phiên MAAPI và giải phóng tài nguyên
 *
 * Quy trình dọn dẹp:
 *   1. Đóng write transaction nếu đang mở (không commit)
 *   2. Kết thúc user session trên ConfD
 *   3. Đóng MAAPI socket
 *   4. Giải phóng bộ nhớ cấu trúc session
 *
 * Tham số:
 *   m — con trỏ tới phiên MAAPI cần đóng (có thể NULL)
 */
void cli_session_close(maapi_session_t *m) {
    if (!m) return;

    /* Đóng write transaction đang mở (nếu có) mà không commit */
    if (m->has_write) {
        maapi_finish_trans(m->sock, m->th_write);
    }

    /* Kết thúc user session và đóng MAAPI socket */
    maapi_end_user_session(m->sock);
    maapi_close(m->sock);   /* Hàm maapi_close(int sock) của ConfD */
    free(m);
}

/* ─── Thao tác đọc cấu hình (Read) ────────────────────────── */

/* Khai báo trước hàm confd_stream_connect từ libconfd — kết nối luồng dữ liệu */
extern int confd_stream_connect(int sock, const struct sockaddr *srv,
                                int srv_sz, int id, int flags);

/*
 * maapi_get_config_xml — Xuất toàn bộ cấu hình từ datastore dưới dạng XML
 *
 * Cơ chế hoạt động:
 *   1. Mở read transaction trên datastore được chỉ định
 *   2. Gọi maapi_save_config() để lấy stream-id
 *   3. Mở socket thứ hai qua confd_stream_connect() để đọc dữ liệu XML
 *   4. Đọc toàn bộ XML từ stream socket vào bộ nhớ
 *
 * Phương pháp stream socket cho phép hoạt động xuyên container
 * (không cần chia sẻ hệ thống file với ConfD).
 *
 * Tham số:
 *   m  — con trỏ tới phiên MAAPI
 *   db — datastore cần đọc (CONFD_RUNNING hoặc CONFD_CANDIDATE)
 *
 * Trả về:
 *   Chuỗi XML chứa toàn bộ cấu hình (malloc'd, caller phải free),
 *   hoặc NULL nếu thất bại.
 */
char *maapi_get_config_xml(maapi_session_t *m, int db) {
    /* Mở read-only transaction trên datastore */
    int th = maapi_start_trans(m->sock, db, CONFD_READ);
    if (th < 0) {
        fprintf(stderr, "[maapi] start_trans(read) failed: %s\n",
                confd_lasterr());
        return NULL;
    }

    /*
     * maapi_save_config trả về stream-id. Mở socket thứ hai
     * với confd_stream_connect(id) để đọc dữ liệu XML.
     * Cách này hoạt động xuyên container (không cần filesystem chung).
     */
    int sid = maapi_save_config(m->sock, th,
                                MAAPI_CONFIG_XML               /* Xuất dạng XML */
                                | MAAPI_CONFIG_WITH_DEFAULTS   /* Bao gồm giá trị mặc định */
                                | MAAPI_CONFIG_SHOW_DEFAULTS,  /* Hiển thị giá trị mặc định */
                                NULL);  /* NULL = không lọc theo path, lấy toàn bộ */
    if (sid < 0) {
        fprintf(stderr, "[maapi] save_config failed: %s\n", confd_lasterr());
        maapi_finish_trans(m->sock, th);
        return NULL;
    }

    /* Mở stream socket tới ConfD để nhận dữ liệu XML */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)m->port);
    inet_aton(m->host, &addr.sin_addr);

    int ssock = socket(AF_INET, SOCK_STREAM, 0);
    if (ssock < 0) {
        maapi_finish_trans(m->sock, th);
        return NULL;
    }

    /* Kết nối stream socket với stream-id vừa nhận */
    if (confd_stream_connect(ssock, (struct sockaddr *)&addr,
                             sizeof(addr), sid, 0) != CONFD_OK) {
        fprintf(stderr, "[maapi] stream_connect failed: %s\n", confd_lasterr());
        close(ssock);
        maapi_finish_trans(m->sock, th);
        return NULL;
    }

    /* Đọc toàn bộ dữ liệu XML từ stream socket vào bộ nhớ */
    size_t cap = 64 * 1024, len = 0;  /* Bắt đầu với buffer 64KB */
    char *raw = malloc(cap);
    if (!raw) { close(ssock); maapi_finish_trans(m->sock, th); return NULL; }

    ssize_t n;
    while ((n = read(ssock, raw + len, cap - len - 1)) > 0) {
        len += (size_t)n;
        /* Tự động mở rộng buffer khi gần đầy (nhân đôi dung lượng) */
        if (len + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(raw, cap);
            if (!tmp) break;
            raw = tmp;
        }
    }
    raw[len] = '\0';
    close(ssock);

    /* Chờ kết quả cuối cùng từ ConfD (xác nhận hoàn tất) */
    maapi_save_config_result(m->sock, sid);
    maapi_finish_trans(m->sock, th);

    if (len == 0) { free(raw); return NULL; }

    /* DEBUG: ghi XML thô ra file tạm để kiểm tra khi cần */
    {
        FILE *dbg = fopen("/tmp/maapi-raw-debug.xml", "w");
        if (dbg) { fwrite(raw, 1, len, dbg); fclose(dbg); }
    }

    /* XML thô đã có wrapper <config xmlns="...">...</config>.
     * Trả về nguyên dạng — formatter xử lý được cả <config> và <data>. */
    return raw;
}

/* ─── Thao tác ghi cấu hình (Write) ───────────────────────── */

/*
 * maapi_set_value_str — Đặt giá trị một leaf node theo keypath
 *
 * Sử dụng maapi_set_elem2() để đặt giá trị dạng chuỗi thuần —
 * ConfD tự động chuyển đổi sang kiểu dữ liệu phù hợp theo YANG schema.
 *
 * Tham số:
 *   m       — con trỏ tới phiên MAAPI
 *   keypath — đường dẫn ConfD tới leaf (ví dụ: "/system/hostname")
 *   value   — giá trị mới dạng chuỗi
 *
 * Trả về:
 *   0 nếu thành công, -1 nếu thất bại.
 */
int maapi_set_value_str(maapi_session_t *m,
                        const char *keypath, const char *value) {
    if (ensure_write_trans(m) != 0) return -1;

    /* maapi_set_elem2 nhận chuỗi thuần — không cần tạo confd_value_t */
    if (maapi_set_elem2(m->sock, m->th_write, value, "%s", keypath) != CONFD_OK) {
        fprintf(stderr, "[maapi] set_elem2 %s = %s failed: %s\n",
                keypath, value, confd_lasterr());
        return -1;
    }
    return 0;
}

/*
 * maapi_load_xml — Nạp cấu hình XML vào candidate datastore (chế độ merge)
 *
 * Ghi XML vào file tạm rồi gọi maapi_load_config() để ConfD đọc.
 * Chế độ MERGE: các node mới được thêm, node trùng được cập nhật,
 * node không có trong XML giữ nguyên.
 *
 * Tham số:
 *   m   — con trỏ tới phiên MAAPI
 *   xml — chuỗi XML cấu hình (nội dung bên trong <config>)
 *
 * Trả về:
 *   0 nếu thành công, -1 nếu thất bại.
 */
int maapi_load_xml(maapi_session_t *m, const char *xml) {
    if (!xml || !*xml) return -1;
    if (ensure_write_trans(m) != 0) return -1;

    /* Ghi XML vào file tạm — maapi_load_config yêu cầu đường dẫn file */
    char *tmppath = write_tmp_str(xml);
    if (!tmppath) return -1;

    /* Nạp cấu hình từ file tạm vào write transaction */
    int rc = maapi_load_config(m->sock, m->th_write,
                               MAAPI_CONFIG_XML | MAAPI_CONFIG_MERGE,
                               tmppath);

    /* Dọn dẹp file tạm ngay sau khi ConfD đã đọc xong */
    unlink(tmppath);
    free(tmppath);

    if (rc != CONFD_OK) {
        fprintf(stderr, "[maapi] load_config failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

/*
 * maapi_delete_node — Xoá một node trong cấu hình theo keypath
 *
 * Xoá node và tất cả con của nó khỏi candidate datastore.
 * Có thể xoá container, list entry, hoặc leaf.
 *
 * Tham số:
 *   m       — con trỏ tới phiên MAAPI
 *   keypath — đường dẫn ConfD tới node cần xoá (ví dụ: "/system/ntp/server{10.0.0.1}")
 *
 * Trả về:
 *   0 nếu thành công, -1 nếu thất bại.
 */
int maapi_delete_node(maapi_session_t *m, const char *keypath) {
    if (ensure_write_trans(m) != 0) return -1;

    if (maapi_delete(m->sock, m->th_write, "%s", keypath) != CONFD_OK) {
        fprintf(stderr, "[maapi] delete %s failed: %s\n",
                keypath, confd_lasterr());
        return -1;
    }
    return 0;
}

/* ─── Quản lý transaction (Transaction control) ───────────── */

/*
 * maapi_do_validate — Xác thực (validate) các thay đổi trong write transaction
 *
 * Chạy kiểm tra ràng buộc YANG (must, when, unique, mandatory...)
 * trên toàn bộ thay đổi trong write transaction hiện tại.
 *
 * Tham số:
 *   m — con trỏ tới phiên MAAPI
 *
 * Trả về:
 *   0 nếu hợp lệ (hoặc không có gì cần validate), -1 nếu có lỗi.
 */
int maapi_do_validate(maapi_session_t *m) {
    if (!m->has_write) return 0; /* Không có thay đổi → không cần validate */

    if (maapi_validate_trans(m->sock, m->th_write,
                             0 /* unlock: không unlock sau validate */,
                             1 /* forcevalidation: bắt buộc validate */)
        != CONFD_OK) {
        fprintf(stderr, "[maapi] validate failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

/*
 * maapi_do_commit — Áp dụng thay đổi từ candidate vào running datastore
 *
 * Quy trình 2 bước:
 *   1. apply_trans: ghi write transaction vào candidate datastore
 *   2. candidate_commit: đẩy candidate → running (cấu hình chạy thực)
 *
 * Sau khi commit thành công, write transaction được đóng.
 *
 * Tham số:
 *   m — con trỏ tới phiên MAAPI
 *
 * Trả về:
 *   0 nếu thành công, -1 nếu thất bại.
 */
int maapi_do_commit(maapi_session_t *m) {
    /* Bước 1: Áp dụng write transaction vào candidate datastore */
    if (m->has_write) {
        if (maapi_apply_trans(m->sock, m->th_write, 0) != CONFD_OK) {
            fprintf(stderr, "[maapi] apply_trans failed: %s\n", confd_lasterr());
            return -1;
        }
        /* Đóng write transaction sau khi đã áp dụng thành công */
        maapi_finish_trans(m->sock, m->th_write);
        m->has_write = false;
    }

    /* Bước 2: Commit candidate → running (cập nhật cấu hình đang chạy) */
    if (maapi_candidate_commit(m->sock) != CONFD_OK) {
        fprintf(stderr, "[maapi] candidate_commit failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

/*
 * maapi_do_discard — Huỷ bỏ mọi thay đổi chưa commit, khôi phục candidate
 *
 * Quy trình:
 *   1. Đóng write transaction hiện tại (drop, không apply)
 *   2. Reset candidate datastore về trạng thái giống running
 *
 * Tham số:
 *   m — con trỏ tới phiên MAAPI
 *
 * Trả về:
 *   0 nếu thành công, -1 nếu thất bại.
 */
int maapi_do_discard(maapi_session_t *m) {
    /* Đóng write transaction mà không apply */
    drop_write_trans(m);

    /* Reset candidate datastore về trạng thái giống running */
    if (maapi_candidate_reset(m->sock) != CONFD_OK) {
        fprintf(stderr, "[maapi] candidate_reset failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

/*
 * maapi_do_lock — Khoá datastore để ngăn phiên khác sửa đổi
 *
 * Tham số:
 *   m  — con trỏ tới phiên MAAPI
 *   db — datastore cần khoá (CONFD_RUNNING hoặc CONFD_CANDIDATE)
 *
 * Trả về:
 *   0 nếu thành công, -1 nếu datastore đã bị khoá bởi phiên khác.
 */
int maapi_do_lock(maapi_session_t *m, int db) {
    if (maapi_lock(m->sock, db) != CONFD_OK) {
        fprintf(stderr, "[maapi] lock failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

/*
 * maapi_do_unlock — Mở khoá datastore đã khoá trước đó
 *
 * Tham số:
 *   m  — con trỏ tới phiên MAAPI
 *   db — datastore cần mở khoá (CONFD_RUNNING hoặc CONFD_CANDIDATE)
 *
 * Trả về:
 *   0 nếu thành công, -1 nếu thất bại (ví dụ: không phải phiên đã khoá).
 */
int maapi_do_unlock(maapi_session_t *m, int db) {
    if (maapi_unlock(m->sock, db) != CONFD_OK) {
        fprintf(stderr, "[maapi] unlock failed: %s\n", confd_lasterr());
        return -1;
    }
    return 0;
}

/* ─── Tải schema từ ConfD ──────────────────────────────────── */

/*
 * maapi_load_schema_into — Tải YANG schema từ ConfD qua kết nối MAAPI hiện có
 *
 * Sử dụng thông tin host/port từ phiên MAAPI đã kết nối
 * để gọi maapi_load_schema() (hàm cấp thấp trong maapi.c).
 *
 * Tham số:
 *   m          — con trỏ tới phiên MAAPI đang mở
 *   out_schema — con trỏ output, sẽ trỏ tới schema tree sau khi tải
 *
 * Trả về:
 *   true nếu tải schema thành công, false nếu thất bại.
 */
bool maapi_load_schema_into(maapi_session_t *m, schema_node_t **out_schema) {
    if (!m || !out_schema) return false;
    return maapi_load_schema(m->host, m->port, out_schema);
}

/* ─── Chuyển đổi đường dẫn (Path conversion) ──────────────── */

/*
 * args_to_keypath — Chuyển đổi mảng token (từ dòng lệnh) thành keypath ConfD
 *
 * Duyệt schema tree để xác định loại từng node:
 *   - Container/leaf: thêm "/<tên>" vào keypath
 *   - List: thêm "/<tên>{<key>}" (token kế tiếp là giá trị key)
 *   - Leaf: dừng lại (phần còn lại trong args là value)
 *
 * Ví dụ:
 *   Input:  args = ["system", "ntp", "server", "10.0.0.1", "prefer"]
 *   Output: "/system/ntp/server{10.0.0.1}/prefer" (consumed = 5)
 *
 * Tham số:
 *   schema   — gốc schema tree để tra cứu loại node
 *   args     — mảng token từ dòng lệnh
 *   argc     — số lượng token
 *   consumed — con trỏ output: số token đã sử dụng cho keypath
 *
 * Trả về:
 *   Chuỗi keypath (malloc'd, caller phải free), hoặc NULL nếu lỗi.
 */
char *args_to_keypath(schema_node_t *schema,
                      char **args, int argc,
                      int *consumed) {
    if (consumed) *consumed = 0;
    if (!schema || !args || argc == 0) return NULL;

    /* Cấp phát buffer đủ lớn cho keypath (tự động mở rộng khi cần) */
    size_t cap = 512;
    char  *kp  = malloc(cap);
    if (!kp) return NULL;
    kp[0] = '\0';
    size_t len = 0;

    schema_node_t *node = schema; /* Bắt đầu duyệt từ gốc schema tree */
    int i = 0;

    while (i < argc) {
        const char *token = args[i];

        /* Tìm node con có tên khớp với token (không phân biệt hoa thường) */
        schema_node_t *child = NULL;
        for (schema_node_t *c = node->children; c; c = c->next) {
            if (strcasecmp(c->name, token) == 0) { child = c; break; }
        }

        if (!child) {
            /* Không tìm thấy token trong schema — vẫn thêm vào keypath
             * để hỗ trợ các node chưa có trong schema (ví dụ: augment) */
            size_t needed = len + 1 + strlen(token) + 1;
            while (needed >= cap) { cap *= 2; kp = realloc(kp, cap); }
            len += (size_t)snprintf(kp + len, cap - len, "/%s", token);
            i++;
            if (consumed) *consumed = i;
            break; /* Không biết loại node → dừng duyệt */
        }

        /* Thêm tên node vào keypath */
        size_t needed = len + 1 + strlen(child->name) + 1;
        while (needed >= cap) { cap *= 2; kp = realloc(kp, cap); }
        len += (size_t)snprintf(kp + len, cap - len, "/%s", child->name);
        i++;

        /* Nếu là YANG list: token tiếp theo là giá trị key → bọc trong {} */
        if (child->is_list && i < argc) {
            const char *key = args[i];
            size_t kneeded = len + 1 + strlen(key) + 2;
            while (kneeded >= cap) { cap *= 2; kp = realloc(kp, cap); }
            len += (size_t)snprintf(kp + len, cap - len, "{%s}", key);
            i++;
        }

        if (consumed) *consumed = i;

        /* Dừng khi gặp leaf — phần token còn lại là giá trị (value) */
        if (child->is_leaf) break;

        /* Di chuyển xuống node con để tiếp tục duyệt */
        node = child;
    }

    return kp; /* Caller phải free */
}

#endif /* WITH_MAAPI */
