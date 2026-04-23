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
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "confd_compat.h"
#include "cli.h"
#include "maapi-direct.h"
#include "log.h"

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
    fchmod(fd, 0644);

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
        LOG_WARN("maapi: start_trans(write) failed: %s", confd_lasterr());
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
        LOG_WARN("maapi: invalid address: %s", host);
        return NULL;
    }

    /* Tạo TCP socket */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return NULL;

    /* Kết nối tới ConfD qua giao thức MAAPI IPC */
    if (maapi_connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != CONFD_OK) {
        LOG_WARN("maapi: connect to %s:%d failed: %s",
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
        LOG_WARN("maapi: start_user_session failed: %s", confd_lasterr());
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
    /* Nếu đang có write-trans mở trên cùng datastore (CANDIDATE sau khi `set`
     * chưa commit), reuse nó để đọc — nếu mở read-trans riêng sẽ không thấy
     * các edit còn đang buffer trong write-trans → candidate nhìn giống hệt
     * running. */
    int  th;
    bool own_trans;
    if (m->has_write && db == CONFD_CANDIDATE) {
        th        = m->th_write;
        own_trans = false;
    } else {
        th = maapi_start_trans(m->sock, db, CONFD_READ);
        if (th < 0) {
            LOG_WARN("maapi: start_trans(read) failed: %s", confd_lasterr());
            return NULL;
        }
        own_trans = true;
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
        LOG_WARN("maapi: save_config failed: %s", confd_lasterr());
        if (own_trans) maapi_finish_trans(m->sock, th);
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
        if (own_trans) maapi_finish_trans(m->sock, th);
        return NULL;
    }

    /* Kết nối stream socket với stream-id vừa nhận */
    if (confd_stream_connect(ssock, (struct sockaddr *)&addr,
                             sizeof(addr), sid, 0) != CONFD_OK) {
        LOG_WARN("maapi: stream_connect failed: %s", confd_lasterr());
        close(ssock);
        if (own_trans) maapi_finish_trans(m->sock, th);
        return NULL;
    }

    /* Đọc toàn bộ dữ liệu XML từ stream socket vào bộ nhớ */
    size_t cap = 64 * 1024, len = 0;  /* Bắt đầu với buffer 64KB */
    char *raw = malloc(cap);
    if (!raw) {
        close(ssock);
        if (own_trans) maapi_finish_trans(m->sock, th);
        return NULL;
    }

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
    if (own_trans) maapi_finish_trans(m->sock, th);

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
        LOG_WARN("maapi: set_elem2 %s = %s failed: %s",
                 keypath, value, confd_lasterr());
        return -1;
    }
    return 0;
}

/*
 * maapi_create_list_entry — Đảm bảo list entry tại keypath tồn tại.
 *
 * ConfD: maapi_create tạo list entry hoặc presence container. Nếu entry đã
 * tồn tại, một số phiên bản trả CONFD_ERR với code "already exists" — ta coi
 * đó là thành công (idempotent). Gọi trước khi set leaves của list entry để
 * phòng trường hợp entry chưa có sẵn.
 *
 * Trả 0 nếu entry đã/đang tồn tại, -1 nếu lỗi thực sự.
 */
int maapi_create_list_entry(maapi_session_t *m, const char *keypath) {
    if (ensure_write_trans(m) != 0) return -1;
    int rc = maapi_create(m->sock, m->th_write, "%s", keypath);
    if (rc == CONFD_OK) return 0;

    /* Entry đã tồn tại → OK, coi như idempotent. Kiểm tra chính xác bằng
     * confd_errno (== CONFD_ERR_ALREADY_EXISTS). Fallback sang so chuỗi
     * với confd_lasterr() vì 1 số phiên bản ConfD đặt errno khác cho cùng
     * tình huống (ví dụ: child leaves đã tồn tại khi set lại list entry). */
    if (confd_errno == CONFD_ERR_ALREADY_EXISTS) return 0;
    const char *err = confd_lasterr();
    if (err && (strstr(err, "exists") || strstr(err, "already"))) {
        return 0;
    }
    LOG_WARN("maapi: create %s failed: errno=%d err=%s",
             keypath, confd_errno, err ? err : "?");
    return -1;
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
        LOG_WARN("maapi: load_config failed: %s", confd_lasterr());
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
        LOG_WARN("maapi: delete %s failed: %s", keypath, confd_lasterr());
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

    /* unlock=1: giữ trans mở cho các thao tác tiếp theo (vd: commit).
     * Theo confd_lib_maapi(3): nếu unlock=0 thì call kế tiếp BẮT BUỘC là
     * maapi_prepare_trans() hoặc maapi_finish_trans() — apply_trans() sẽ fail.
     * forcevalidation=1: bắt buộc validate trên candidate datastore. */
    if (maapi_validate_trans(m->sock, m->th_write,
                             1 /* unlock */,
                             1 /* forcevalidation */)
        != CONFD_OK) {
        LOG_WARN("maapi: validate failed: %s", confd_lasterr());
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
            LOG_WARN("maapi: apply_trans failed: %s", confd_lasterr());
            return -1;
        }
        /* Đóng write transaction sau khi đã áp dụng thành công */
        maapi_finish_trans(m->sock, m->th_write);
        m->has_write = false;
    }

    /* Bước 2: Commit candidate → running (cập nhật cấu hình đang chạy) */
    if (maapi_candidate_commit(m->sock) != CONFD_OK) {
        LOG_WARN("maapi: candidate_commit failed: %s", confd_lasterr());
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
        LOG_WARN("maapi: candidate_reset failed: %s", confd_lasterr());
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
        LOG_WARN("maapi: lock failed: %s", confd_lasterr());
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
        LOG_WARN("maapi: unlock failed: %s", confd_lasterr());
        return -1;
    }
    return 0;
}

/* ─── Rollback ────────────────────────────────────────────── */

/*
 * maapi_do_list_rollbacks — Liệt kê các rollback file ConfD đang lưu.
 *
 * Trả về số entry đã ghi vào `out` (≤ max), -1 nếu lỗi.
 * Mảng out đã được lọc/sắp xếp theo thứ tự ConfD trả về (mới nhất trước).
 */
int maapi_do_list_rollbacks(maapi_session_t *m,
                            struct rollback_entry *out, int max) {
    if (!m || !out || max <= 0) return -1;

    struct maapi_rollback raw[64];
    int cap = max < 64 ? max : 64;
    int n   = cap;  /* IN: capacity, OUT: actual count */
    if (maapi_list_rollbacks(m->sock, raw, &n) != CONFD_OK) {
        LOG_WARN("maapi: list_rollbacks failed: %s", confd_lasterr());
        return -1;
    }
    if (n > cap) n = cap;

    for (int i = 0; i < n; i++) {
        out[i].nr       = raw[i].nr;
        out[i].fixed_nr = raw[i].fixed_nr;
        snprintf(out[i].creator, sizeof(out[i].creator), "%s", raw[i].creator);
        snprintf(out[i].datestr, sizeof(out[i].datestr), "%s", raw[i].datestr);
        snprintf(out[i].via,     sizeof(out[i].via),     "%s", raw[i].via);
        snprintf(out[i].label,   sizeof(out[i].label),   "%s", raw[i].label);
        snprintf(out[i].comment, sizeof(out[i].comment), "%s", raw[i].comment);
    }
    return n;
}

/*
 * maapi_do_load_rollback — Stage một rollback file vào candidate.
 *
 * Mở write trans (nếu chưa có), gọi maapi_load_rollback(num) trên trans đó.
 * Nội dung rollback được merge vào candidate; người dùng cần `commit` để
 * áp dụng vào running.
 */
int maapi_do_load_rollback(maapi_session_t *m, int rollback_num) {
    if (!m) return -1;
    if (ensure_write_trans(m) != 0) return -1;

    if (maapi_load_rollback(m->sock, m->th_write, rollback_num) != CONFD_OK) {
        LOG_WARN("maapi: load_rollback %d failed: %s",
                 rollback_num, confd_lasterr());
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

/* args_to_keypath đã tách sang src/args_util.c để unit-test được mà không
 * phải link libconfd — hàm thuần xử lý schema, không dùng MAAPI. */

#endif /* WITH_MAAPI */
