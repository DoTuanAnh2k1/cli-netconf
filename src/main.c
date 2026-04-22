/*
 * main.c — CLI NETCONF via ConfD MAAPI
 *
 * Tệp chính của chương trình CLI NETCONF. Chương trình kết nối trực tiếp
 * tới cổng IPC của ConfD (mặc định :4565) thông qua giao diện MAAPI (Management
 * Agent API). Tất cả các thao tác get-config / set / commit đều đi qua MAAPI,
 * không cần qua NETCONF/SSH.
 *
 * Build:
 *   make CONFD_DIR=/path/to/confd
 *
 * Run:
 *   CONFD_IPC_ADDR=127.0.0.1 CONFD_IPC_PORT=4565 ./cli-netconf
 *
 * Biến môi trường (Environment variables):
 *   CONFD_IPC_ADDR   Địa chỉ ConfD    (mặc định: 127.0.0.1)
 *   CONFD_IPC_PORT   Cổng ConfD        (mặc định: 4565)
 *   MAAPI_USER       Tên người dùng    (mặc định: admin)
 *   NE_NAME          Nhãn hiển thị trên prompt (mặc định: confd)
 *
 * Danh sách lệnh hỗ trợ:
 *   show running-config [path...]
 *   show candidate-config [path...]
 *   set <path...> <value>     — đặt giá trị leaf (phân tách bằng dấu cách hoặc /keypath)
 *   set                       — dán khối cấu hình XML (dòng trống để kết thúc)
 *   unset <path...>           — xoá node
 *   commit
 *   validate
 *   discard
 *   lock [running|candidate]
 *   unlock [running|candidate]
 *   dump xml [file]
 *   dump text [file]
 *   help
 *   exit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include "confd_compat.h"
#include <readline/readline.h>
#include <readline/history.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "cli.h"
#include "log.h"
#include "maapi-direct.h"

/* ─── Forward declarations ─────────────────────────────────── */
static int select_and_connect_ne(void);

/* Guard: kiểm tra MAAPI session trước khi chạy command */
#define REQUIRE_MAAPI() do { \
    if (!g_maapi) { \
        fprintf(stderr, "%sNot connected.%s Use %slogin%s to connect to a NE.\n", \
                COLOR_RED, COLOR_RESET, COLOR_CYAN, COLOR_RESET); \
        return; \
    } \
} while (0)

/* ─── Biến toàn cục (Globals) ───────────────────────────────── */

/* Con trỏ tới phiên MAAPI hiện tại, dùng chung cho toàn bộ chương trình */
static maapi_session_t *g_maapi   = NULL;

/* Cây schema YANG đã tải từ ConfD, phục vụ tab-completion và chuyển đổi đường dẫn */
static schema_node_t   *g_schema  = NULL;

/* Chuỗi prompt hiển thị trên dòng lệnh readline */
static char             g_prompt[512];

/* Tên thiết bị mạng (Network Element), hiển thị trong prompt */
static char             g_ne_name[128] = "confd";

/* Token lấy từ mgt-svc sau khi login (response_data). ĐÃ BAO GỒM tiền tố
 * "Basic " theo spec API — dùng trực tiếp làm giá trị header Authorization.
 * Rỗng nếu chưa login. */
static char             g_mgt_token[4096] = "";

/* Username đang đăng nhập (để hiển thị trên prompt sau login). */
static char             g_mgt_user[128] = "";

/* Username dùng trong prompt CLI — lấy từ MAAPI_USER/USER env tại startup. */
static char             g_cli_user[128] = "admin";

/* Địa chỉ client gửi SSH (parse từ env SSH_CLIENT = "ip port port"), rỗng nếu
 * không có (direct mode hoặc chạy local). Dùng để chèn vào log command tracing
 * nên admin biết ai/ở đâu gõ lệnh gì. */
static char             g_rhost[64] = "";

/* Set khi user gõ "exit"/"quit" ở màn chọn NE → thoát hẳn program thay vì
 * quay lại login / show NE list lại. Ctrl+D vẫn dùng chung nhánh "sel < 0"
 * nhưng không set flag → giữ được hành vi cancel cũ nơi cần. */
static int              g_quit_requested = 0;

/* Cache của candidate XML dành cho tab-completion (collect_list_keys). Tránh
 * round-trip MAAPI mỗi lần bấm TAB. Bị invalidate sau mọi lệnh có thể đổi
 * candidate: set / unset / commit / discard / rollback. */
static char            *g_xml_cache = NULL;

static void invalidate_xml_cache(void) {
    free(g_xml_cache);
    g_xml_cache = NULL;
}

/**
 * init_rhost - Khởi tạo g_rhost từ biến môi trường SSH_CLIENT.
 *
 * SSH_CLIENT có định dạng "<remote_ip> <remote_port> <local_port>".
 * Lấy phần đầu (IP) lưu vào g_rhost. Nếu không có biến này (chạy local/direct)
 * thì g_rhost vẫn rỗng.
 */
static void init_rhost(void) {
    const char *sc = getenv("SSH_CLIENT");
    if (!sc || !*sc) { g_rhost[0] = '\0'; return; }
    const char *sp = strchr(sc, ' ');
    size_t len = sp ? (size_t)(sp - sc) : strlen(sc);
    if (len >= sizeof(g_rhost)) len = sizeof(g_rhost) - 1;
    memcpy(g_rhost, sc, len);
    g_rhost[len] = '\0';
}

/* ─── Hàm tiện ích (Helpers) ─────────────────────────────────── */

/**
 * env_or - Lấy giá trị biến môi trường, trả về giá trị mặc định nếu không tồn tại.
 *
 * @param k  Tên biến môi trường cần đọc
 * @param d  Giá trị mặc định nếu biến không tồn tại hoặc rỗng
 * @return   Giá trị biến môi trường hoặc giá trị mặc định
 */
static const char *env_or(const char *k, const char *d) {
    const char *v = getenv(k); return (v && *v) ? v : d;
}

/**
 * env_int_or - Lấy giá trị biến môi trường dưới dạng số nguyên.
 *
 * @param k  Tên biến môi trường cần đọc
 * @param d  Giá trị mặc định (int) nếu biến không tồn tại hoặc rỗng
 * @return   Giá trị số nguyên của biến môi trường hoặc giá trị mặc định
 */
static int env_int_or(const char *k, int d) {
    const char *v = getenv(k); return (v && *v) ? atoi(v) : d;
}

/**
 * elapsed_ms - Tính thời gian đã trôi qua tính bằng mili-giây kể từ mốc t0.
 *
 * Dùng để đo hiệu năng các thao tác MAAPI (get-config, commit, v.v.).
 *
 * @param t0  Con trỏ tới mốc thời gian bắt đầu (struct timeval)
 * @return    Số mili-giây đã trôi qua
 */
static long elapsed_ms(struct timeval *t0) {
    struct timeval now; gettimeofday(&now, NULL);
    return (now.tv_sec - t0->tv_sec) * 1000 +
           (now.tv_usec - t0->tv_usec) / 1000;
}

/**
 * print_done - In footer chung sau khi lệnh kết thúc: duration + timestamp.
 * Format: "(12ms, 2026-04-18 14:30:45)" bằng COLOR_DIM.
 */
static void print_done(long ms) {
    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
    printf("%s(%ldms, %s)%s\n", COLOR_DIM, ms, ts, COLOR_RESET);
}

/**
 * sigint_handler - Xử lý tín hiệu SIGINT (Ctrl+C).
 *
 * Thay vì thoát chương trình, handler này xoá dòng lệnh hiện tại
 * và hiển thị lại prompt trống, giống hành vi của các shell thông thường.
 *
 * @param sig  Mã tín hiệu (không sử dụng)
 */
static void sigint_handler(int sig) {
    (void)sig;
    printf("\n");
    rl_on_new_line();          /* Báo readline rằng con trỏ đã xuống dòng mới */
    rl_replace_line("", 0);   /* Xoá nội dung dòng lệnh hiện tại */
    rl_redisplay();            /* Vẽ lại prompt trống */
}

/**
 * update_prompt - Cập nhật chuỗi prompt hiển thị trên dòng lệnh.
 *
 * Định dạng: "<user>[<tên_thiết_bị>]> " với màu sắc ANSI.
 * <user> lấy từ g_cli_user (MAAPI_USER/USER env tại startup).
 */
static void update_prompt(void) {
    snprintf(g_prompt, sizeof(g_prompt),
             "%s%s%s[%s%s%s]> ",
             COLOR_CYAN, g_cli_user, COLOR_RESET,
             COLOR_YELLOW, g_ne_name, COLOR_RESET);
}

/**
 * get_terminal_rows - Lấy số dòng hiện tại của cửa sổ terminal.
 *
 * Sử dụng ioctl TIOCGWINSZ để truy vấn kích thước terminal.
 * Nếu không lấy được (ví dụ đầu ra không phải terminal), trả về 24 dòng mặc định.
 *
 * @return  Số dòng của terminal
 */
static int get_terminal_rows(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row;
    return 24; /* Giá trị dự phòng nếu không đọc được kích thước terminal */
}

/**
 * paged_print - In nội dung text ra stdout với phân trang (pager) đơn giản.
 *
 * Khi số dòng vượt quá chiều cao terminal, hiển thị prompt <MORE> cho phép
 * người dùng chọn: [Enter] xem thêm một trang, [a] xem tất cả, [q] thoát.
 *
 * @param text  Chuỗi văn bản cần in (có thể chứa nhiều dòng)
 */
static void paged_print(const char *text) {
    if (!text) return;
    int page = get_terminal_rows() - 1; /* Trừ 1 dòng dành cho prompt <MORE> */
    if (page < 5) page = 5;             /* Đảm bảo tối thiểu 5 dòng mỗi trang */

    /* Đặt terminal vào cbreak mode: đọc 1 phím không cần Enter, không echo.
     * Nhờ vậy bấm phím không đẩy con trỏ xuống → <MORE> luôn ở dòng cuối. */
    struct termios old_tio, new_tio;
    int raw_ok = (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_tio) == 0);
    if (raw_ok) {
        new_tio = old_tio;
        new_tio.c_lflag &= ~(ICANON | ECHO);
        new_tio.c_cc[VMIN]  = 1;
        new_tio.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    }

    const char *p = text;
    int lines = 0;
    while (*p) {
        /* Tìm vị trí xuống dòng tiếp theo và in từng dòng một */
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p + 1) : strlen(p);
        fwrite(p, 1, len, stdout);
        p += len;
        lines++;

        /* Khi đã in đủ một trang và vẫn còn nội dung, hỏi người dùng */
        if (lines >= page && *p) {
            lines = 0;
            /* Đếm số dòng còn lại chưa hiển thị */
            int rem = 0; const char *q = p;
            while (*q) { if (*q == '\n') rem++; q++; }
            printf("%s<MORE>%s [Enter] next [a] all [q] quit — %d lines left ",
                   COLOR_YELLOW, COLOR_RESET, rem);
            fflush(stdout);
            int ch = getchar();
            /* Xoá dòng <MORE> ngay tại chỗ: \r về đầu dòng, \033[2K xoá toàn bộ dòng.
             * Vì đã tắt ECHO, bấm phím không in ký tự nào → con trỏ vẫn ở dòng <MORE>. */
            fputs("\r\033[2K", stdout);
            fflush(stdout);
            if (ch == 'q' || ch == 'Q' || ch == 27 || ch == EOF) {
                if (raw_ok) tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
                return;
            }
            if (ch == 'a' || ch == 'A' || ch == 'G') {
                /* In toàn bộ phần còn lại, không pager nữa */
                fputs(p, stdout);
                if (raw_ok) tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
                return;
            }
            /* Mặc định (Enter/Space/phím khác): tiếp tục trang kế */
        }
    }

    if (raw_ok) tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
}

/* ─── Tab completion (Tự động hoàn thành bằng phím Tab) ──────── */

/**
 * cmd_generator - Hàm sinh gợi ý cho tên lệnh (từ đầu tiên trên dòng lệnh).
 *
 * Được gọi bởi readline khi người dùng nhấn Tab ở vị trí từ đầu tiên.
 * Trả về lần lượt các lệnh khớp với tiền tố `text`.
 *
 * @param text   Tiền tố mà người dùng đã gõ
 * @param state  0 = lần gọi đầu tiên (khởi tạo), != 0 = lần gọi tiếp theo
 * @return       Chuỗi gợi ý (malloc'd) hoặc NULL khi hết gợi ý
 */
static char *cmd_generator(const char *text, int state) {
    static const char *cmds[] = {
        "show", "set", "unset", "commit", "validate",
        "discard", "lock", "unlock", "dump", "help", "exit", NULL
    };
    static int idx;
    if (!state) idx = 0;  /* Lần gọi đầu: đặt lại chỉ mục về 0 */
    while (cmds[idx]) {
        const char *c = cmds[idx++];
        /* So sánh không phân biệt hoa/thường với tiền tố người dùng đã gõ */
        if (strncasecmp(c, text, strlen(text)) == 0)
            return strdup(c);
    }
    return NULL;
}

/*
 * Duyệt cây schema bằng tất cả các token đã hoàn thành trên dòng lệnh
 * (trước từ đang gõ) để tìm node cha phù hợp.
 * Sau đó gợi ý các node con của node cha đó khớp với `text`.
 */

/* Node cha dùng cho completion hiện tại */
static schema_node_t *g_comp_parent = NULL;

/**
 * find_completion_parent - Tìm node cha trong cây schema dựa trên đường dẫn
 * mà người dùng đã gõ trên dòng lệnh.
 *
 * Hàm phân tích nội dung dòng lệnh readline, tách thành các token,
 * bỏ qua tên lệnh (show/set/unset) và lệnh con (running-config/candidate-config),
 * rồi duyệt cây schema theo các token còn lại để xác định node cha hiện tại.
 *
 * @return  Con trỏ tới node schema cha, hoặc NULL nếu không tìm được
 */
static schema_node_t *find_completion_parent(void) {
    if (!g_schema) return NULL;

    /* Lấy toàn bộ nội dung dòng lệnh tính đến vị trí con trỏ */
    const char *line = rl_line_buffer;
    if (!line) return g_schema;

    /* Tách dòng lệnh thành mảng token */
    int count = 0;
    char **tokens = str_split(line, &count);
    if (!tokens || count == 0) {
        free_tokens(tokens, count);
        return g_schema;
    }

    /* Bỏ qua token đầu tiên (tên lệnh: show, set, unset) */
    int start_idx = 1;
    /* Nếu lệnh là "show", bỏ qua thêm token thứ 2 (running-config/candidate-config) */
    if (count > 1 && (strcasecmp(tokens[0], "show") == 0)) {
        start_idx = 2;
    }

    /* Duyệt cây schema theo các token đường dẫn, bỏ qua token cuối nếu đang gõ dở */
    schema_node_t *node = g_schema;
    int end_idx = count;
    /* Nếu dòng lệnh không kết thúc bằng dấu cách, token cuối là từ đang gõ dở (partial) */
    int line_len = strlen(line);
    if (line_len > 0 && line[line_len - 1] != ' ') {
        end_idx = count - 1;
    }

    for (int i = start_idx; i < end_idx && node; i++) {
        /* Tìm node con có tên khớp với token hiện tại */
        schema_node_t *found = NULL;
        for (schema_node_t *c = node->children; c; c = c->next) {
            if (strcasecmp(c->name, tokens[i]) == 0) {
                found = c;
                break;
            }
        }
        if (found) {
            node = found;
            /* Nếu node là list (danh sách YANG), token tiếp theo là giá trị key -> bỏ qua */
            if (found->is_list && i + 1 < end_idx) {
                i++; /* Nhảy qua giá trị key của list */
            }
        } else {
            /* Token không khớp node con nào — có thể là giá trị key, dừng duyệt */
            break;
        }
    }

    free_tokens(tokens, count);
    return node;
}

/*
 * collect_list_keys — Fetch candidate XML và extract key value của các entry
 *                     đang tồn tại dưới list `list_node`. Dùng cho tab
 *                     completion: khi user ở vị trí chọn list entry, hiện
 *                     danh sách key đã có (giống ConfD CLI).
 *
 * Thuật toán:
 *   1. Parse tokens của rl_line_buffer (bỏ show/running-config ở đầu).
 *   2. Walk XML doc theo tokens (bỏ token cuối = tên list), đến được
 *      element parent của list entries.
 *   3. Iterate children với name = list_node->name, extract element con
 *      đầu tiên (= key leaf) → copy value.
 *
 * Trả về số key copy được. Các string trong out[] là malloc'd, caller
 * chịu trách nhiệm free.
 */
static int collect_list_keys(schema_node_t *list_node, char **out, int max) {
    if (!g_maapi || !list_node || !list_node->is_list || !rl_line_buffer)
        return 0;

    /* Dùng cache nếu có; nếu chưa có thì fetch 1 lần rồi giữ cho lần TAB sau.
     * Cache bị xoá bởi invalidate_xml_cache() sau các lệnh thay đổi candidate. */
    if (!g_xml_cache) {
        g_xml_cache = maapi_get_config_xml(g_maapi, CONFD_CANDIDATE);
    }
    if (!g_xml_cache) return 0;

    xmlDocPtr doc = xmlReadMemory(g_xml_cache, (int)strlen(g_xml_cache), "c.xml", NULL,
                                   XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (!doc) return 0;

    int count = 0;
    char **tokens = str_split(rl_line_buffer, &count);
    if (!tokens) { xmlFreeDoc(doc); return 0; }

    int start_idx = 1;
    if (count > 1 && strcasecmp(tokens[0], "show") == 0) start_idx = 2;

    int line_len = (int)strlen(rl_line_buffer);
    int end_idx  = count;
    if (line_len > 0 && rl_line_buffer[line_len - 1] != ' ')
        end_idx = count - 1;  /* token cuối là partial, bỏ qua */

    /* Walk XML từ root, bỏ token cuối (tên list), chỉ đến parent của list */
    xmlNodePtr cur = xmlDocGetRootElement(doc);  /* <config xmlns=...> */
    for (int i = start_idx; i < end_idx - 1 && cur; i++) {
        xmlNodePtr next = NULL;
        for (xmlNodePtr c = cur->children; c; c = c->next) {
            if (c->type == XML_ELEMENT_NODE &&
                strcasecmp((char *)c->name, tokens[i]) == 0) {
                next = c; break;
            }
        }
        cur = next;
    }
    if (!cur) {
        xmlFreeDoc(doc);
        free_tokens(tokens, count);
        return 0;
    }

    /* Iterate children với tên list_node->name, lấy element con đầu tiên làm key */
    int n = 0;
    for (xmlNodePtr c = cur->children; c && n < max; c = c->next) {
        if (c->type != XML_ELEMENT_NODE) continue;
        if (strcasecmp((char *)c->name, list_node->name) != 0) continue;
        for (xmlNodePtr k = c->children; k; k = k->next) {
            if (k->type != XML_ELEMENT_NODE) continue;
            xmlChar *val = xmlNodeGetContent(k);
            if (val && *val) {
                out[n++] = xstrdup((char *)val);
            }
            if (val) xmlFree(val);
            break;  /* chỉ lấy leaf đầu tiên = key */
        }
    }

    xmlFreeDoc(doc);
    free_tokens(tokens, count);
    return n;
}

/* Bộ đệm completion: schema children + (nếu parent là list) key values */
static char **g_path_items_buf = NULL;
static int   *g_path_items_is_key = NULL;   /* parallel flag: 1 nếu item là key value */
static int    g_path_items_count = 0;
static int    g_path_items_idx   = 0;

static void free_path_items(void) {
    if (!g_path_items_buf) return;
    for (int i = 0; i < g_path_items_count; i++) free(g_path_items_buf[i]);
    free(g_path_items_buf);
    free(g_path_items_is_key);
    g_path_items_buf = NULL;
    g_path_items_is_key = NULL;
    g_path_items_count = 0;
    g_path_items_idx = 0;
}

/*
 * path_display_hook — Hook hiển thị tùy biến cho tab completion (readline).
 *
 * Chia matches thành 2 khu riêng:
 *   - Possible completions (leaf/container) — tên schema children
 *   - Possible key completions           — giá trị key của list entry đang có
 *
 * Hook chỉ kích hoạt khi có ít nhất 1 key trong g_path_items; ngược lại để
 * readline render mặc định. Khớp matches[1..num] với g_path_items_buf bằng
 * so sánh chuỗi (vì matches[0] là "longest common prefix" do readline chèn).
 */
static int g_path_items_has_keys(void) {
    if (!g_path_items_is_key) return 0;
    for (int i = 0; i < g_path_items_count; i++)
        if (g_path_items_is_key[i]) return 1;
    return 0;
}

static int lookup_is_key(const char *name) {
    if (!g_path_items_is_key) return 0;
    for (int i = 0; i < g_path_items_count; i++) {
        if (g_path_items_buf[i] && strcmp(g_path_items_buf[i], name) == 0)
            return g_path_items_is_key[i];
    }
    return 0;
}

/* In matches theo columns dựa trên độ rộng terminal — thay thế việc gọi
 * rl_display_match_list từ trong hook (sẽ recurse vào chính hook này). */
static void print_matches_columns(char **matches, int start, int end, int max_length) {
    if (start > end) return;
    int cols = get_terminal_rows();  /* fallback if ioctl fails */
    struct winsize ws;
    int cw = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) cw = ws.ws_col;
    (void)cols;
    int col_w = max_length + 2;
    if (col_w < 4) col_w = 4;
    int per_row = cw / col_w;
    if (per_row < 1) per_row = 1;

    int c = 0;
    for (int i = start; i <= end; i++) {
        const char *s = matches[i];
        int sl = (int)strlen(s);
        fputs(s, stdout);
        for (int k = sl; k < col_w; k++) fputc(' ', stdout);
        c++;
        if (c >= per_row) { fputs("\r\n", stdout); c = 0; }
    }
    if (c != 0) fputs("\r\n", stdout);
}

static void path_display_hook(char **matches, int num, int max_length) {
    /* QUAN TRỌNG: không được gọi rl_display_match_list() từ đây — hàm đó
     * tự check hook và call lại chính hook này → infinite recursion, dẫn tới
     * completion list in 2-3 lần và làm mất dòng gõ của user. */
    rl_crlf();

    int has_keys = g_path_items_has_keys();
    if (!has_keys) {
        /* Không có key → in flat như readline mặc định */
        print_matches_columns(matches, 1, num, max_length);
    } else {
        /* Có cả schema children và key value → chia 2 section */
        int leaf_start = -1, leaf_end = -1, key_start = -1, key_end = -1;
        /* Xếp vào 2 mảng tạm (stack) để in từng section cạnh cột */
        char **leaves = malloc(sizeof(char *) * (num + 1));
        char **keys   = malloc(sizeof(char *) * (num + 1));
        int nl = 0, nk = 0;
        int max_leaf = 0, max_key = 0;
        for (int i = 1; i <= num; i++) {
            int slen = (int)strlen(matches[i]);
            if (lookup_is_key(matches[i])) {
                keys[++nk] = matches[i];
                if (slen > max_key) max_key = slen;
            } else {
                leaves[++nl] = matches[i];
                if (slen > max_leaf) max_leaf = slen;
            }
        }
        (void)leaf_start; (void)leaf_end; (void)key_start; (void)key_end;

        if (nl > 0) {
            printf("%sPossible completions:%s\r\n", COLOR_BOLD, COLOR_RESET);
            print_matches_columns(leaves, 1, nl, max_leaf);
        }
        if (nk > 0) {
            printf("%sPossible match completions:%s\r\n", COLOR_BOLD, COLOR_RESET);
            /* Key in màu cyan, mỗi dòng một key cho dễ đọc */
            for (int i = 1; i <= nk; i++)
                printf("  " COLOR_CYAN "%s" COLOR_RESET "\r\n", keys[i]);
        }
        free(leaves);
        free(keys);
    }
    fflush(stdout);
    /* Báo readline: cursor ở dòng trống mới → ép vẽ lại prompt + input để
     * user không mất dòng đang gõ. Thiếu 2 call này là lý do bấm TAB xong
     * dòng command biến mất. */
    rl_on_new_line();
    rl_forced_update_display();
}

/**
 * path_generator - Hàm sinh gợi ý cho đường dẫn schema YANG.
 *
 * Xây dựng danh sách completion ở state==0: bao gồm tên các schema children
 * của parent, VÀ nếu parent là YANG list thì cả các key value đang tồn tại
 * trong candidate config (giống ConfD CLI `show full-configuration ... <list>`
 * trả về các entry đã tạo).
 *
 * @param text   Tiền tố mà người dùng đã gõ
 * @param state  0 = lần gọi đầu tiên, != 0 = lần gọi tiếp theo
 * @return       Chuỗi gợi ý (malloc'd) hoặc NULL khi hết
 */
static char *path_generator(const char *text, int state) {
    if (!state) {
        free_path_items();
        g_comp_parent = find_completion_parent();
        if (!g_comp_parent) return NULL;

        int cap = 64;
        g_path_items_buf    = malloc(sizeof(char *) * cap);
        g_path_items_is_key = malloc(sizeof(int)    * cap);
        if (!g_path_items_buf || !g_path_items_is_key) return NULL;

        /* 1) Schema children names (không phải key) */
        for (schema_node_t *c = g_comp_parent->children; c; c = c->next) {
            if (g_path_items_count + 1 >= cap) {
                cap *= 2;
                char **nb = realloc(g_path_items_buf, sizeof(char *) * cap);
                int   *kb = realloc(g_path_items_is_key, sizeof(int) * cap);
                if (!nb || !kb) break;
                g_path_items_buf    = nb;
                g_path_items_is_key = kb;
            }
            g_path_items_is_key[g_path_items_count] = 0;
            g_path_items_buf[g_path_items_count++] = xstrdup(c->name);
        }

        /* 2) Nếu parent là list → thêm key value hiện có (đánh dấu is_key=1) */
        if (g_comp_parent->is_list) {
            char *keys[256];
            int nk = collect_list_keys(g_comp_parent, keys, 256);
            for (int i = 0; i < nk; i++) {
                if (g_path_items_count + 1 >= cap) {
                    cap *= 2;
                    char **nb = realloc(g_path_items_buf, sizeof(char *) * cap);
                    int   *kb = realloc(g_path_items_is_key, sizeof(int) * cap);
                    if (!nb || !kb) { free(keys[i]); continue; }
                    g_path_items_buf    = nb;
                    g_path_items_is_key = kb;
                }
                g_path_items_is_key[g_path_items_count] = 1;
                g_path_items_buf[g_path_items_count++] = keys[i];  /* đã xstrdup */
            }
        }
    }

    size_t tlen = strlen(text);
    while (g_path_items_idx < g_path_items_count) {
        const char *it = g_path_items_buf[g_path_items_idx++];
        if (strncasecmp(it, text, tlen) == 0)
            return strdup(it);
    }
    return NULL;
}

/*
 * Hàm sinh gợi ý tổng quát — duyệt qua một danh sách chuỗi tĩnh.
 * Danh sách được đặt vào g_list_items trước khi gọi rl_completion_matches.
 */

/* Con trỏ tới mảng chuỗi gợi ý hiện tại */
static const char **g_list_items = NULL;
/* Chỉ mục duyệt trong mảng gợi ý */
static int g_list_idx = 0;

/**
 * list_generator - Hàm sinh gợi ý tổng quát từ danh sách g_list_items.
 *
 * @param text   Tiền tố người dùng đã gõ
 * @param state  0 = lần gọi đầu tiên, != 0 = lần gọi tiếp theo
 * @return       Chuỗi gợi ý (malloc'd) hoặc NULL khi hết
 */
static char *list_generator(const char *text, int state) {
    if (!state) g_list_idx = 0;
    if (!g_list_items) return NULL;
    while (g_list_items[g_list_idx]) {
        const char *s = g_list_items[g_list_idx++];
        if (strncasecmp(s, text, strlen(text)) == 0)
            return strdup(s);
    }
    return NULL;
}

/**
 * maapi_completer - Hàm callback chính cho tab-completion của readline.
 *
 * Xác định ngữ cảnh hiện tại (đang gõ lệnh gì, ở vị trí nào) để chọn
 * hàm generator phù hợp:
 *   - Vị trí từ đầu tiên: gợi ý tên lệnh
 *   - Sau "show": gợi ý running-config / candidate-config, rồi đường dẫn schema
 *   - Sau "set" / "unset": gợi ý đường dẫn schema
 *   - Sau "dump": gợi ý định dạng (text / xml)
 *   - Sau "lock" / "unlock": gợi ý datastore (running / candidate)
 *
 * @param text   Tiền tố từ đang được hoàn thành
 * @param start  Vị trí bắt đầu của từ trong rl_line_buffer
 * @param end    Vị trí kết thúc của từ trong rl_line_buffer (không sử dụng)
 * @return       Mảng chuỗi gợi ý (malloc'd) hoặc NULL
 */
static char **maapi_completer(const char *text, int start, int end) {
    (void)end;
    /* Ngăn readline dùng completion mặc định (tên file) khi không có gợi ý */
    rl_attempted_completion_over = 1;

    /* Từ đầu tiên trên dòng lệnh: gợi ý tên lệnh */
    if (start == 0)
        return rl_completion_matches(text, cmd_generator);

    /* Phân tích các token trên dòng lệnh để xác định ngữ cảnh */
    int count = 0;
    char **tokens = str_split(rl_line_buffer, &count);
    if (!tokens || count == 0) {
        free_tokens(tokens, count);
        return NULL;
    }
    const char *cmd = tokens[0];
    /* Kiểm tra xem con trỏ có đang sau dấu cách cuối (trailing space) hay không */
    int line_len = (int)strlen(rl_line_buffer);
    int trailing = (line_len > 0 && rl_line_buffer[line_len - 1] == ' ');
    int word_count = trailing ? count + 1 : count; /* Vị trí từ logic (đang gõ từ thứ mấy) */

    char **result = NULL;

    if (strcasecmp(cmd, "show") == 0) {
        if (word_count == 2) {
            /* "show <TAB>" hoặc "show run<TAB>" -> gợi ý lệnh con (running-config / candidate-config) */
            static const char *subs[] = {"running-config", "candidate-config", NULL};
            g_list_items = subs;
            result = rl_completion_matches(text, list_generator);
        } else {
            /* "show running-config <TAB>" -> gợi ý đường dẫn schema */
            result = rl_completion_matches(text, path_generator);
        }
    } else if (strcasecmp(cmd, "set") == 0) {
        /* Sau "set": duyệt schema xem có chạm vào YANG list hay không.
         *
         * Cú pháp batch set list:
         *   set <container...> <list-name> <key-value> [<leaf> <value> ...]
         *
         * Khi đã qua tên list → vị trí tiếp theo là key VALUE (do user tự
         * gõ, không gợi ý). Sau key value là các cặp <leaf> <value> lặp lại;
         * gợi ý chỉ xuất hiện ở vị trí leaf name, và chỉ liệt kê những leaf
         *   - không phải key của list (key đã gõ qua key value)
         *   - chưa xuất hiện trên dòng lệnh hiện tại
         */
        int confirmed = trailing ? count : count - 1;
        int cur_idx   = trailing ? count : count - 1;  /* 0-based vị trí word đang gõ */

        schema_node_t *n = g_schema;
        schema_node_t *list_node = NULL;
        int list_tok_idx = -1;
        for (int i = 1; i < confirmed && n; i++) {
            schema_node_t *found = NULL;
            for (schema_node_t *c = n->children; c; c = c->next) {
                if (strcasecmp(c->name, tokens[i]) == 0) { found = c; break; }
            }
            if (!found) break;
            n = found;
            if (found->is_list) {
                list_node = found;
                list_tok_idx = i;
                break;  /* Không đi sâu qua list — các token sau là key/leaf */
            }
        }

        if (list_node && list_tok_idx >= 0) {
            int rel = cur_idx - list_tok_idx;
            if (rel == 1) {
                /* Key value — user tự gõ, không gợi ý */
                result = NULL;
            } else if (rel >= 2 && (rel % 2) == 0) {
                /* Leaf name — build danh sách leaf chưa dùng, không phải key */
                static const char *buf[64];
                int bi = 0;
                for (schema_node_t *c = list_node->children; c && bi < 63; c = c->next) {
                    if (!c->is_leaf) continue;
                    if (schema_is_key_leaf(list_node, c->name)) continue;
                    bool used = false;
                    for (int k = list_tok_idx + 2; k < confirmed; k += 2) {
                        if (strcasecmp(tokens[k], c->name) == 0) { used = true; break; }
                    }
                    if (used) continue;
                    buf[bi++] = c->name;
                }
                buf[bi] = NULL;
                g_list_items = buf;
                result = rl_completion_matches(text, list_generator);
            } else {
                /* Leaf value (rel lẻ >= 3) → không gợi ý */
                result = NULL;
            }
        } else {
            result = rl_completion_matches(text, path_generator);
        }
    } else if (strcasecmp(cmd, "unset") == 0) {
        /* Unset vẫn dùng path_generator như cũ */
        result = rl_completion_matches(text, path_generator);
    } else if (strcasecmp(cmd, "dump") == 0) {
        /* Sau "dump": gợi ý định dạng xuất (text / xml) */
        static const char *fmts[] = {"text", "xml", NULL};
        g_list_items = fmts;
        result = rl_completion_matches(text, list_generator);
    } else if (strcasecmp(cmd, "lock") == 0 ||
               strcasecmp(cmd, "unlock") == 0) {
        /* Sau "lock"/"unlock": gợi ý datastore (running / candidate) */
        static const char *dss[] = {"running", "candidate", NULL};
        g_list_items = dss;
        result = rl_completion_matches(text, list_generator);
    }

    free_tokens(tokens, count);
    return result;
}

/* ─── Xử lý lệnh (Command handlers) ────────────────────────── */

/**
 * cmd_show - Xử lý lệnh "show running-config" hoặc "show candidate-config".
 *
 * Đọc cấu hình từ datastore chỉ định qua MAAPI, chuyển đổi XML sang dạng text
 * có thụt lề, lọc theo đường dẫn (nếu có), và hiển thị qua pager.
 *
 * @param args  Mảng tham số (args[0] = "running-config"/"candidate-config", args[1..] = đường dẫn lọc)
 * @param argc  Số lượng tham số
 */
static void cmd_show(char **args, int argc) {
    REQUIRE_MAAPI();
    if (argc == 0) {
        printf("Usage: show running-config | candidate-config [path...]\n");
        return;
    }

    /* Xác định datastore: running hoặc candidate */
    int db;
    if (strcasecmp(args[0], "running-config") == 0)       db = CONFD_RUNNING;
    else if (strcasecmp(args[0], "candidate-config") == 0) db = CONFD_CANDIDATE;
    else {
        printf("Unknown: %s\n", args[0]);
        return;
    }

    /* Đo thời gian thực hiện lệnh get-config */
    struct timeval t0; gettimeofday(&t0, NULL);
    char *xml = maapi_get_config_xml(g_maapi, db);
    long ms = elapsed_ms(&t0);

    const char *who = (*g_mgt_user) ? g_mgt_user : g_cli_user;
    if (!xml) {
        LOG_WARN("show FAILED: user=%s ne=%s ds=%s", who, g_ne_name, args[0]);
        fprintf(stderr, "%sget-config failed%s\n", COLOR_RED, COLOR_RESET);
        return;
    }
    LOG_INFO("show OK: user=%s ne=%s ds=%s (%ldms)",
             who, g_ne_name, args[0], ms);

    /* Lọc XML theo đường dẫn nếu người dùng chỉ định (ví dụ: show running-config system ntp) */
    const char **path = (const char **)(args + 1);
    int path_len = argc - 1;

    /* Chuyển đổi XML thô thành dạng text có thụt lề dễ đọc */
    char *text = fmt_xml_to_text(xml, path, path_len);
    free(xml);

    /* Dòng đầu = path filter (bỏ tên datastore — không in "running-config"/
     * "candidate-config"). Không có filter → không có dòng path.
     * Các dòng sau = cây config (mỗi cấp = 1 tab). */
    char *header = NULL;
    if (path_len > 0) {
        size_t header_cap = 2;
        for (int i = 0; i < path_len; i++) header_cap += strlen(path[i]) + 1;
        header = malloc(header_cap);
        if (header) {
            size_t off = 0;
            for (int i = 0; i < path_len; i++) {
                off += snprintf(header + off, header_cap - off,
                                "%s%s", i > 0 ? " " : "", path[i]);
            }
            snprintf(header + off, header_cap - off, "\n");
        }
    }

    size_t tlen = text ? strlen(text) : 0;
    size_t hlen = header ? strlen(header) : 0;
    char *combined = malloc(hlen + tlen + 1);
    if (combined) {
        if (header) memcpy(combined, header, hlen);
        if (text)   memcpy(combined + hlen, text, tlen);
        combined[hlen + tlen] = '\0';
        paged_print(combined);
        free(combined);
    } else {
        paged_print(text);
    }
    free(header);
    free(text);
    print_done(ms);
}

/**
 * read_xml_paste - Đọc khối XML cấu hình từ stdin (chế độ paste).
 *
 * Hiển thị prompt yêu cầu người dùng dán nội dung XML, đọc từng dòng
 * cho đến khi gặp dòng trống. Bộ đệm được tự động mở rộng khi cần.
 *
 * @return  Chuỗi XML đã đọc (malloc'd, người gọi phải free), hoặc NULL nếu lỗi
 */
static char *read_xml_paste(void) {
    printf("Paste XML config (empty line to finish):\n");
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return NULL;
    buf[0] = '\0';

    char *line;
    while ((line = readline("")) != NULL) {
        if (*line == '\0') { free(line); break; }  /* Dòng trống: kết thúc nhập liệu */
        size_t ll = strlen(line) + 1;
        /* Mở rộng bộ đệm nếu không đủ chỗ chứa dòng mới */
        while (len + ll + 2 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(line); free(buf); return NULL; }
            buf = nb;
        }
        /* Nối dòng vào bộ đệm, thêm ký tự xuống dòng */
        memcpy(buf + len, line, ll - 1);
        len += ll - 1;
        buf[len++] = '\n';
        buf[len] = '\0';
        free(line);
    }
    return buf;
}

/**
 * cmd_set - Xử lý lệnh "set" để đặt giá trị cấu hình.
 *
 * Hỗ trợ hai chế độ:
 *   1. Không có tham số: chuyển sang chế độ paste XML (gọi read_xml_paste)
 *   2. Có tham số: đặt giá trị leaf theo đường dẫn
 *
 * Đường dẫn hỗ trợ hai cú pháp:
 *   - Phân tách bằng dấu cách: "set system ntp enabled true" (chuyển đổi qua schema)
 *   - Keypath ConfD: "set /system/ntp/enabled true" (truyền thẳng)
 *
 * @param args  Mảng tham số (đường dẫn + giá trị)
 * @param argc  Số lượng tham số (0 = chế độ paste XML)
 */
/*
 * print_children_hint — In danh sách các con của một schema node ra stderr.
 *
 * Dùng để cung cấp gợi ý schema-aware khi user gõ sai cú pháp set/unset:
 * nhờ đó user biết được tại vị trí hiện tại có thể gõ tiếp những tên nào,
 * và loại của từng tên (leaf / list / container).
 */
static void print_children_hint(schema_node_t *node) {
    if (!node || !node->children) return;
    fprintf(stderr, "  %savailable%s: ", COLOR_DIM, COLOR_RESET);
    bool first = true;
    for (schema_node_t *c = node->children; c; c = c->next) {
        /* Ẩn key leaf khi node là list — key đã được cung cấp qua key value
         * trong keypath ({john}), user không nên set lại leaf key bằng tên. */
        if (node->is_list && schema_is_key_leaf(node, c->name)) continue;
        if (!first) fprintf(stderr, ", ");
        const char *tag = c->is_list ? " [list]"
                       : c->is_leaf ? ""
                       : " [container]";
        fprintf(stderr, "%s%s", c->name, tag);
        first = false;
    }
    fprintf(stderr, "\n");
}

/*
 * do_single_set — Đặt 1 leaf duy nhất. Log + in kết quả.
 * Trả về 0 nếu OK, -1 nếu fail.
 */
static int do_single_set(const char *kp, const char *value, const char *who) {
    if (maapi_set_value_str(g_maapi, kp, value) == 0) {
        LOG_INFO("set OK: user=%s ne=%s path=%s value=%s",
                 who, g_ne_name, kp, value);
        printf("%sOK%s %s %s=%s %s\n",
               COLOR_GREEN, COLOR_RESET, kp, COLOR_DIM, value, COLOR_RESET);
        return 0;
    }
    LOG_WARN("set FAILED: user=%s ne=%s path=%s value=%s",
             who, g_ne_name, kp, value);
    fprintf(stderr, "%sset failed%s %s\n", COLOR_RED, COLOR_RESET, kp);
    return -1;
}

/**
 * cmd_set — Xử lý lệnh "set". Hỗ trợ 3 chế độ:
 *
 *   1. Paste XML (argc == 0): đọc khối XML từ stdin, nạp vào candidate.
 *
 *   2. Keypath ConfD trực tiếp:
 *        set /system/hostname edge-01
 *        set /system/ntp/server{10.0.0.1}/prefer true
 *
 *   3. Token phân tách bằng dấu cách, duyệt schema để suy ra keypath:
 *        set system hostname edge-01
 *        set system ntp enabled true
 *
 *      Với YANG list, cú pháp là:
 *        set <container...> <list-name> <key-value> [<leaf> <value> ...]
 *      Ví dụ `list subscriber { key name; leaf role; leaf email; }`:
 *        set eir subscriber john role admin
 *        set eir subscriber john role admin email john@x.com   <- batch nhiều leaf
 *      Không cần gõ tên key (`name`) thêm lần nữa vì `john` đã làm key.
 *
 * Lỗi cú pháp → in gợi ý schema-aware (các tên con tại vị trí hiện tại).
 */
static void cmd_set(char **args, int argc) {
    REQUIRE_MAAPI();
    const char *who = (*g_mgt_user) ? g_mgt_user : g_cli_user;

    /* ── Chế độ paste ─ chấp nhận cả XML và output `show running-config` ──
     * Nếu ký tự non-whitespace đầu tiên là '<' → XML, nạp trực tiếp.
     * Ngược lại → text (định dạng như show running-config), convert
     * sang XML qua fmt_text_to_xml(content, g_schema).
     */
    if (argc == 0) {
        char *content = read_xml_paste();
        if (!content) return;

        const char *p = content;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        bool is_xml = (*p == '<');

        char *xml = is_xml ? xstrdup(content) : fmt_text_to_xml(content, g_schema);
        free(content);
        if (!xml) {
            fprintf(stderr, "%sfailed to convert pasted text to XML%s\n",
                    COLOR_RED, COLOR_RESET);
            return;
        }

        if (maapi_load_xml(g_maapi, xml) == 0) {
            LOG_INFO("set OK (paste %s): user=%s ne=%s",
                     is_xml ? "XML" : "text", who, g_ne_name);
            printf("%sOK%s (staged in candidate)\n", COLOR_GREEN, COLOR_RESET);
        } else {
            LOG_WARN("set FAILED (paste %s): user=%s ne=%s",
                     is_xml ? "XML" : "text", who, g_ne_name);
            fprintf(stderr, "%sload failed%s\n", COLOR_RED, COLOR_RESET);
        }
        free(xml);
        return;
    }

    /* ── Chế độ keypath ConfD trực tiếp ───────────────────────── */
    if (args[0][0] == '/') {
        if (argc < 2) {
            fprintf(stderr,
                "%sUsage (keypath)%s: set <keypath> <value>\n"
                "  eg: set /system/hostname edge-01\n"
                "      set /system/ntp/server{10.0.0.1}/prefer true\n",
                COLOR_RED, COLOR_RESET);
            return;
        }
        do_single_set(args[0], args[1], who);
        return;
    }

    /* ── Chế độ token: duyệt schema tree ─────────────────────── */
    size_t cap = 512;
    char  *kp  = malloc(cap);
    if (!kp) return;
    size_t len = 0;
    kp[0] = '\0';

    schema_node_t *node = g_schema;   /* Node hiện tại trong schema */
    int i = 0;

    while (i < argc) {
        const char *token = args[i];

        /* Tìm child có tên khớp (case-insensitive) */
        schema_node_t *child = NULL;
        for (schema_node_t *c = node->children; c; c = c->next) {
            if (strcasecmp(c->name, token) == 0) { child = c; break; }
        }

        if (!child) {
            fprintf(stderr, "%sUnknown node '%s'%s at path %s\n",
                    COLOR_RED, token, COLOR_RESET, *kp ? kp : "/");
            print_children_hint(node);
            free(kp);
            return;
        }

        /* Nối tên node vào keypath */
        size_t needed = len + 1 + strlen(child->name) + 1;
        while (needed >= cap) { cap *= 2; kp = realloc(kp, cap); }
        len += (size_t)snprintf(kp + len, cap - len, "/%s", child->name);
        i++;

        /* ── LEAF: token kế tiếp là giá trị, kết thúc ──────── */
        if (child->is_leaf) {
            if (i >= argc) {
                fprintf(stderr,
                    "%sMissing value for leaf '%s'%s\n"
                    "  eg: set ... %s <value>\n",
                    COLOR_RED, child->name, COLOR_RESET, child->name);
                free(kp);
                return;
            }
            if (i + 1 < argc) {
                fprintf(stderr,
                    "%sLeaf '%s' chỉ nhận 1 giá trị, có thừa: '%s'...%s\n"
                    "  eg: set ... %s <value>\n",
                    COLOR_RED, child->name, args[i+1], COLOR_RESET,
                    child->name);
                free(kp);
                return;
            }
            do_single_set(kp, args[i], who);
            free(kp);
            return;
        }

        /* ── LIST: token kế tiếp là key value ──────────────── */
        if (child->is_list) {
            if (i >= argc) {
                fprintf(stderr,
                    "%sMissing key value for list '%s'%s\n"
                    "  eg: set %s <key> [<leaf> <value> ...]\n",
                    COLOR_RED, child->name, COLOR_RESET, kp);
                print_children_hint(child);
                free(kp);
                return;
            }
            const char *key = args[i];
            size_t kneeded = len + strlen(key) + 3;
            while (kneeded >= cap) { cap *= 2; kp = realloc(kp, cap); }
            len += (size_t)snprintf(kp + len, cap - len, "{%s}", key);
            i++;
            node = child;  /* Xuống list entry (children = leaves của list) */

            /* Không còn arg → chỉ tạo entry rỗng → không khả thi qua set đơn.
             * Yêu cầu user cung cấp ít nhất 1 leaf/value. */
            if (i >= argc) {
                fprintf(stderr,
                    "%sList entry '%s{%s}' cần ít nhất 1 leaf/value%s\n"
                    "  eg: set %s <leaf> <value> [<leaf> <value> ...]\n",
                    COLOR_RED, child->name, key, COLOR_RESET, kp);
                print_children_hint(child);
                free(kp);
                return;
            }

            /* Peek token kế tiếp: nếu là LEAF con của list → batch mode.
             * Nếu là container / nested list → tiếp tục duyệt xuống. */
            schema_node_t *peek = NULL;
            for (schema_node_t *c = child->children; c; c = c->next) {
                if (strcasecmp(c->name, args[i]) == 0) { peek = c; break; }
            }

            if (peek && peek->is_leaf) {
                /* ── BATCH: remaining = <leaf> <value> cặp đôi ─ */
                int remaining = argc - i;
                if (remaining % 2 != 0) {
                    fprintf(stderr,
                        "%sCần số chẵn token (cặp <leaf> <value>), có %d%s\n"
                        "  eg: set %s role admin email john@x.com\n",
                        COLOR_RED, remaining, COLOR_RESET, kp);
                    print_children_hint(child);
                    free(kp);
                    return;
                }

                /* Đảm bảo list entry tồn tại trước khi set leaves — nếu
                 * entry chưa có, maapi_set_elem2 trên leaf của nó có thể
                 * fail với "path not exists". maapi_create_list_entry
                 * idempotent (đã tồn tại cũng OK). */
                if (maapi_create_list_entry(g_maapi, kp) != 0) {
                    fprintf(stderr, "%screate list entry %s failed%s\n",
                            COLOR_RED, kp, COLOR_RESET);
                    free(kp);
                    return;
                }

                int ok = 0, fail = 0;
                for (int j = i; j + 1 < argc; j += 2) {
                    const char *lname = args[j];
                    const char *lval  = args[j+1];

                    /* Kiểm tra leaf có tồn tại trong list */
                    schema_node_t *lc = NULL;
                    for (schema_node_t *c = child->children; c; c = c->next) {
                        if (strcasecmp(c->name, lname) == 0) { lc = c; break; }
                    }
                    if (!lc || !lc->is_leaf) {
                        fprintf(stderr,
                            "%s'%s' không phải leaf trong list '%s'%s\n",
                            COLOR_RED, lname, child->name, COLOR_RESET);
                        print_children_hint(child);
                        fail++;
                        continue;
                    }
                    /* Không cho set lại key leaf — đã cung cấp qua key value */
                    if (schema_is_key_leaf(child, lc->name)) {
                        fprintf(stderr,
                            "%s'%s' là key của list '%s' — đã được set qua key value, bỏ qua%s\n",
                            COLOR_RED, lc->name, child->name, COLOR_RESET);
                        fail++;
                        continue;
                    }

                    /* Ghép full keypath = kp + "/" + leaf_name */
                    size_t fp_cap = len + strlen(lc->name) + 2;
                    char *fp = malloc(fp_cap);
                    if (!fp) { fail++; continue; }
                    snprintf(fp, fp_cap, "%s/%s", kp, lc->name);

                    if (do_single_set(fp, lval, who) == 0) ok++;
                    else fail++;
                    free(fp);
                }

                if (fail == 0)
                    printf("%sDone%s — %d leaf(s) set under %s\n",
                           COLOR_GREEN, COLOR_RESET, ok, kp);
                else
                    printf("%s%d OK, %d failed%s under %s\n",
                           COLOR_YELLOW, ok, fail, COLOR_RESET, kp);
                free(kp);
                return;
            }

            /* peek không phải leaf → tiếp tục vòng lặp, sẽ duyệt nested node
             * (container/nested list) ở iteration kế tiếp. */
            continue;
        }

        /* ── CONTAINER: đi sâu xuống ───────────────────────── */
        node = child;
    }

    /* Thoát loop mà chưa gặp leaf/list-với-value → đường dẫn chưa đủ */
    fprintf(stderr,
        "%sĐường dẫn chưa đầy đủ — cần trỏ tới leaf hoặc list%s\n"
        "  at: %s\n",
        COLOR_RED, COLOR_RESET, *kp ? kp : "/");
    print_children_hint(node);
    free(kp);
}

/**
 * cmd_unset - Xử lý lệnh "unset" để xoá một node trong cấu hình.
 *
 * Hỗ trợ hai cú pháp đường dẫn:
 *   - Phân tách bằng dấu cách: "unset system ntp server 10.0.0.1"
 *   - Keypath ConfD: "unset /system/ntp/server{10.0.0.1}"
 *
 * @param args  Mảng tham số (đường dẫn tới node cần xoá)
 * @param argc  Số lượng tham số
 */
static void cmd_unset(char **args, int argc) {
    REQUIRE_MAAPI();
    if (argc == 0) {
        printf("Usage: unset <path...>\n"
               "Example: unset system ntp server 10.0.0.1\n"
               "         unset /system/ntp/server{10.0.0.1}  (keypath)\n");
        return;
    }

    char *keypath = NULL;
    if (args[0][0] == '/') {
        /* Cú pháp keypath ConfD: dùng trực tiếp */
        keypath = strdup(args[0]);
    } else {
        /* Cú pháp dấu cách: chuyển đổi qua schema thành keypath */
        int consumed = 0;
        keypath = args_to_keypath(g_schema, args, argc, &consumed);
        if (!keypath || consumed == 0) {
            fprintf(stderr, "%sPath not found in schema%s\n",
                    COLOR_RED, COLOR_RESET);
            free(keypath);
            return;
        }
    }

    /* Gọi MAAPI để xoá node khỏi candidate datastore */
    const char *who = (*g_mgt_user) ? g_mgt_user : g_cli_user;
    if (maapi_delete_node(g_maapi, keypath) == 0) {
        LOG_INFO("unset OK: user=%s ne=%s path=%s", who, g_ne_name, keypath);
        printf("%sOK%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        LOG_WARN("unset FAILED: user=%s ne=%s path=%s",
                 who, g_ne_name, keypath);
        fprintf(stderr, "%sunset failed%s\n", COLOR_RED, COLOR_RESET);
    }
    free(keypath);
}

/**
 * cmd_commit - Xử lý lệnh "commit": áp dụng thay đổi từ candidate sang running.
 *
 * Đo thời gian thực hiện và hiển thị kết quả (thành công hoặc thất bại).
 */
static void cmd_commit(void) {
    REQUIRE_MAAPI();
    struct timeval t0; gettimeofday(&t0, NULL);
    if (maapi_do_commit(g_maapi) == 0) {
        long ms = elapsed_ms(&t0);
        LOG_INFO("commit OK: user=%s ne=%s (%ldms)",
                 (*g_mgt_user) ? g_mgt_user : g_cli_user, g_ne_name, ms);
        printf("%sCommit successful.%s\n", COLOR_GREEN, COLOR_RESET);
        print_done(ms);
    } else {
        LOG_WARN("commit FAILED: user=%s ne=%s",
                 (*g_mgt_user) ? g_mgt_user : g_cli_user, g_ne_name);
        fprintf(stderr, "%sCommit failed.%s\n", COLOR_RED, COLOR_RESET);
    }
}

/**
 * cmd_validate - Xử lý lệnh "validate": kiểm tra tính hợp lệ của candidate.
 *
 * Gọi MAAPI validate để ConfD kiểm tra ràng buộc YANG (must, when, unique, v.v.)
 * mà không áp dụng thay đổi.
 */
static void cmd_validate(void) {
    REQUIRE_MAAPI();
    const char *who = (*g_mgt_user) ? g_mgt_user : g_cli_user;
    if (maapi_do_validate(g_maapi) == 0) {
        LOG_INFO("validate OK: user=%s ne=%s", who, g_ne_name);
        printf("%sValidation OK.%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        LOG_WARN("validate FAILED: user=%s ne=%s", who, g_ne_name);
        fprintf(stderr, "%sValidation failed.%s\n", COLOR_RED, COLOR_RESET);
    }
}

/**
 * cmd_discard - Xử lý lệnh "discard": huỷ bỏ mọi thay đổi trong candidate.
 *
 * Đặt lại candidate datastore về trạng thái giống running datastore.
 */
static void cmd_discard(void) {
    REQUIRE_MAAPI();
    const char *who = (*g_mgt_user) ? g_mgt_user : g_cli_user;
    if (maapi_do_discard(g_maapi) == 0) {
        LOG_INFO("discard OK: user=%s ne=%s", who, g_ne_name);
        printf("%sDiscarded.%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        LOG_WARN("discard FAILED: user=%s ne=%s", who, g_ne_name);
        fprintf(stderr, "%sDiscard failed.%s\n", COLOR_RED, COLOR_RESET);
    }
}

/**
 * cmd_lock - Xử lý lệnh "lock": khoá datastore để ngăn phiên khác chỉnh sửa.
 *
 * Mặc định khoá candidate datastore. Nếu tham số là "running" thì khoá running.
 *
 * @param args  Mảng tham số (tuỳ chọn: "running" hoặc "candidate")
 * @param argc  Số lượng tham số
 */
static void cmd_lock(char **args, int argc) {
    REQUIRE_MAAPI();
    int db = CONFD_CANDIDATE;  /* Mặc định khoá candidate */
    if (argc > 0 && strcasecmp(args[0], "running") == 0) db = CONFD_RUNNING;
    const char *who = (*g_mgt_user) ? g_mgt_user : g_cli_user;
    const char *ds  = (db == CONFD_RUNNING) ? "running" : "candidate";
    if (maapi_do_lock(g_maapi, db) == 0) {
        LOG_INFO("lock OK: user=%s ne=%s ds=%s", who, g_ne_name, ds);
        printf("%sLocked.%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        LOG_WARN("lock FAILED: user=%s ne=%s ds=%s", who, g_ne_name, ds);
        fprintf(stderr, "%sLock failed.%s\n", COLOR_RED, COLOR_RESET);
    }
}

/**
 * cmd_unlock - Xử lý lệnh "unlock": mở khoá datastore đã khoá trước đó.
 *
 * Mặc định mở khoá candidate datastore. Nếu tham số là "running" thì mở khoá running.
 *
 * @param args  Mảng tham số (tuỳ chọn: "running" hoặc "candidate")
 * @param argc  Số lượng tham số
 */
static void cmd_unlock(char **args, int argc) {
    REQUIRE_MAAPI();
    int db = CONFD_CANDIDATE;  /* Mặc định mở khoá candidate */
    if (argc > 0 && strcasecmp(args[0], "running") == 0) db = CONFD_RUNNING;
    const char *who = (*g_mgt_user) ? g_mgt_user : g_cli_user;
    const char *ds  = (db == CONFD_RUNNING) ? "running" : "candidate";
    if (maapi_do_unlock(g_maapi, db) == 0) {
        LOG_INFO("unlock OK: user=%s ne=%s ds=%s", who, g_ne_name, ds);
        printf("%sUnlocked.%s\n", COLOR_GREEN, COLOR_RESET);
    } else {
        LOG_WARN("unlock FAILED: user=%s ne=%s ds=%s", who, g_ne_name, ds);
        fprintf(stderr, "%sUnlock failed.%s\n", COLOR_RED, COLOR_RESET);
    }
}

/**
 * cmd_dump - Xử lý lệnh "dump": xuất toàn bộ cấu hình running ra text hoặc XML.
 *
 * Nếu có chỉ định tên file, ghi kết quả vào file. Nếu không, hiển thị qua pager.
 *
 * @param args  Mảng tham số (args[0] = "text"/"xml" tuỳ chọn, args[1] = tên file tuỳ chọn)
 * @param argc  Số lượng tham số
 */
static void cmd_dump(char **args, int argc) {
    REQUIRE_MAAPI();
    const char *fmt  = (argc > 0) ? args[0] : "text";   /* Mặc định xuất dạng text */
    const char *file = (argc > 1) ? args[1] : NULL;     /* Tên file đích (tuỳ chọn) */

    /* Đọc toàn bộ cấu hình running dưới dạng XML */
    char *xml  = maapi_get_config_xml(g_maapi, CONFD_RUNNING);
    if (!xml) { fprintf(stderr, "%sdump failed%s\n", COLOR_RED, COLOR_RESET); return; }

    char *out;
    if (strcasecmp(fmt, "xml") == 0) {
        /* Giữ nguyên XML thô */
        out = xstrdup(xml);
    } else {
        /* Chuyển đổi sang dạng text có thụt lề */
        out = fmt_xml_to_text(xml, NULL, 0);
    }
    free(xml);
    if (!out) return;

    if (file && *file) {
        /* Ghi kết quả vào file */
        FILE *f = fopen(file, "w");
        if (!f) {
            fprintf(stderr, "%sCannot open %s%s\n", COLOR_RED, file, COLOR_RESET);
        } else {
            fputs(out, f);
            fclose(f);
            printf("Saved to %s%s%s\n", COLOR_CYAN, file, COLOR_RESET);
        }
    } else {
        /* Hiển thị qua pager trên terminal */
        paged_print(out);
    }
    free(out);
}

/* ─── mgt-svc HTTP client ─────────────────────────────────── */

/*
 * parse_url — Tách URL "http://host[:port]/path" thành 3 phần.
 * Chỉ hỗ trợ HTTP (không HTTPS). Trả về 0 nếu OK, -1 nếu lỗi.
 */
static int parse_url(const char *url, char *host, size_t hsz,
                     int *port, char *path, size_t psz) {
    const char *p = url;
    if (strncmp(p, "http://", 7) != 0) return -1;
    p += 7;

    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    size_t hostlen;

    if (colon && (!slash || colon < slash)) {
        hostlen = (size_t)(colon - p);
        *port = atoi(colon + 1);
    } else {
        hostlen = slash ? (size_t)(slash - p) : strlen(p);
        *port = 80;
    }
    if (hostlen == 0 || hostlen >= hsz) return -1;
    memcpy(host, p, hostlen);
    host[hostlen] = '\0';
    snprintf(path, psz, "%s", slash ? slash : "/");
    return 0;
}

/*
 * json_escape — Escape chuỗi để nhúng vào JSON.
 * Trả về malloc'd string, caller phải free.
 */
static char *json_escape(const char *s) {
    if (!s) return strdup("");
    size_t cap = strlen(s) * 6 + 1;
    char *out = malloc(cap);
    if (!out) return NULL;
    char *q = out;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
            case '"':  *q++ = '\\'; *q++ = '"';  break;
            case '\\': *q++ = '\\'; *q++ = '\\'; break;
            case '\n': *q++ = '\\'; *q++ = 'n';  break;
            case '\r': *q++ = '\\'; *q++ = 'r';  break;
            case '\t': *q++ = '\\'; *q++ = 't';  break;
            default:
                if (c < 0x20) q += sprintf(q, "\\u%04x", c);
                else          *q++ = (char)c;
        }
    }
    *q = '\0';
    return out;
}

/* Timeout cho HTTP client (giây) — đảm bảo không bao giờ hang CLI */
#define MGT_HTTP_CONNECT_TIMEOUT_S  3
#define MGT_HTTP_IO_TIMEOUT_S       5

/*
 * connect_with_timeout — connect() có giới hạn thời gian.
 * Trả về 0 OK, -1 lỗi (errno set: ETIMEDOUT, ECONNREFUSED…).
 */
static int connect_with_timeout(int sock, const struct sockaddr *addr,
                                socklen_t alen, int timeout_s) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) return -1;

    int rc = connect(sock, addr, alen);
    if (rc == 0) {
        fcntl(sock, F_SETFL, flags);
        return 0;
    }
    if (errno != EINPROGRESS) return -1;

    struct pollfd pfd = { .fd = sock, .events = POLLOUT };
    int pr = poll(&pfd, 1, timeout_s * 1000);
    if (pr <= 0) {
        if (pr == 0) errno = ETIMEDOUT;
        return -1;
    }

    int err = 0; socklen_t elen = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) return -1;
    if (err != 0) { errno = err; return -1; }

    fcntl(sock, F_SETFL, flags);
    return 0;
}

/*
 * http_post_json — POST một body JSON tới url. Trả về HTTP status code,
 * hoặc -1 nếu không kết nối được (đã in lỗi ra stderr; không bao giờ raise
 * signal hay làm crash app).
 *
 * Bảo đảm:
 *   - SIGPIPE không bao giờ kill app (dùng MSG_NOSIGNAL khi gửi)
 *   - Kết nối có timeout (3s) — không hang trên host không phản hồi
 *   - Đọc/ghi có timeout (5s) — không treo khi server câm
 *   - Mọi lỗi malloc/parse được xử lý sạch, không leak
 *
 * extra_headers (có thể NULL): chuỗi đã kết thúc bằng "\r\n", mỗi header một dòng.
 */
static int http_post_json(const char *url, const char *body,
                          const char *extra_headers, char **resp_out) {
    if (resp_out) *resp_out = NULL;
    if (!url || !*url || !body) return -1;

    char host[256], path[1024];
    int  port;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        fprintf(stderr, "[mgt-svc] bad url: %s\n", url);
        return -1;
    }

    /* DNS resolve — getaddrinfo có thể chậm nhưng không vô tận với numeric/local */
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) {
        fprintf(stderr, "[mgt-svc] resolve %s failed: %s\n",
                host, gai_strerror(gai));
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        fprintf(stderr, "[mgt-svc] socket: %s\n", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    if (connect_with_timeout(sock, res->ai_addr, res->ai_addrlen,
                             MGT_HTTP_CONNECT_TIMEOUT_S) != 0) {
        fprintf(stderr, "[mgt-svc] connect %s:%d failed: %s\n",
                host, port, strerror(errno));
        close(sock); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    /* Set IO timeouts để read()/send() không bao giờ block vĩnh viễn */
    struct timeval tv = { .tv_sec = MGT_HTTP_IO_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Build & send request */
    size_t blen = strlen(body);
    char hdr[2048];
    int hlen = snprintf(hdr, sizeof(hdr),
        "POST %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n", path, host, port, blen,
        extra_headers ? extra_headers : "");
    if (hlen < 0 || hlen >= (int)sizeof(hdr)) {
        fprintf(stderr, "[mgt-svc] header too large\n");
        close(sock); return -1;
    }

    /* MSG_NOSIGNAL: không bao giờ raise SIGPIPE nếu peer đóng giữa chừng */
    if (send(sock, hdr, (size_t)hlen, MSG_NOSIGNAL) < 0 ||
        send(sock, body, blen,        MSG_NOSIGNAL) < 0) {
        fprintf(stderr, "[mgt-svc] send failed: %s\n", strerror(errno));
        close(sock); return -1;
    }

    /* Read response (cap 4KB, có timeout per-recv) */
    char buf[4096];
    size_t got = 0;
    ssize_t n;
    while (got < sizeof(buf) - 1 &&
           (n = recv(sock, buf + got, sizeof(buf) - 1 - got, 0)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';
    close(sock);

    /* Parse "HTTP/1.1 NNN ..." */
    int status = -1;
    if (got > 12 && strncmp(buf, "HTTP/", 5) == 0) {
        const char *sp = strchr(buf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    /* Find body (after \r\n\r\n) */
    if (resp_out) {
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            *resp_out = strdup(body_start);
        } else {
            *resp_out = strdup(buf);
        }
    }
    return status;
}

/*
 * http_get_json — GET request tới url, trả về HTTP status code.
 * Tương tự http_post_json nhưng dùng method GET (không có body).
 *
 * extra_headers: chuỗi "\r\n"-terminated (có thể NULL).
 * resp_out:      nếu != NULL, nhận malloc'd response body (caller free).
 * Trả về HTTP status hoặc -1 nếu lỗi kết nối.
 */
static int http_get_json(const char *url, const char *extra_headers,
                         char **resp_out) {
    if (resp_out) *resp_out = NULL;
    if (!url || !*url) return -1;

    char host[256], path[1024];
    int  port;
    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        fprintf(stderr, "[mgt-svc] bad url: %s\n", url);
        return -1;
    }

    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0 || !res) {
        fprintf(stderr, "[mgt-svc] resolve %s failed: %s\n",
                host, gai_strerror(gai));
        if (res) freeaddrinfo(res);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        fprintf(stderr, "[mgt-svc] socket: %s\n", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }

    if (connect_with_timeout(sock, res->ai_addr, res->ai_addrlen,
                             MGT_HTTP_CONNECT_TIMEOUT_S) != 0) {
        fprintf(stderr, "[mgt-svc] connect %s:%d failed: %s\n",
                host, port, strerror(errno));
        close(sock); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);

    struct timeval tv = { .tv_sec = MGT_HTTP_IO_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char hdr[2048];
    int hlen = snprintf(hdr, sizeof(hdr),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n", path, host, port,
        extra_headers ? extra_headers : "");
    if (hlen < 0 || hlen >= (int)sizeof(hdr)) {
        fprintf(stderr, "[mgt-svc] header too large\n");
        close(sock); return -1;
    }

    if (send(sock, hdr, (size_t)hlen, MSG_NOSIGNAL) < 0) {
        fprintf(stderr, "[mgt-svc] send failed: %s\n", strerror(errno));
        close(sock); return -1;
    }

    /* Read response — dùng buffer lớn hơn vì list NE có thể dài */
    size_t cap = 16384, got = 0;
    char *buf = malloc(cap);
    if (!buf) { close(sock); return -1; }
    ssize_t n;
    while (got < cap - 1 &&
           (n = recv(sock, buf + got, cap - 1 - got, 0)) > 0) {
        got += (size_t)n;
    }
    buf[got] = '\0';
    close(sock);

    int status = -1;
    if (got > 12 && strncmp(buf, "HTTP/", 5) == 0) {
        const char *sp = strchr(buf, ' ');
        if (sp) status = atoi(sp + 1);
    }
    if (resp_out) {
        char *body_start = strstr(buf, "\r\n\r\n");
        if (body_start) {
            body_start += 4;
            *resp_out = strdup(body_start);
        } else {
            *resp_out = strdup(buf);
        }
    }
    free(buf);
    return status;
}

/*
 * mgt_base_url — URL gốc của mgt-svc (env MGT_SVC_BASE).
 * Mặc định: http://127.0.0.1:8080
 */
static const char *mgt_base_url(void) {
    return env_or("MGT_SVC_BASE", "http://127.0.0.1:8080");
}

/*
 * mgt_endpoint — Ghép base URL với path cố định, ghi vào buf.
 * Caller phải cung cấp buf đủ lớn (PATH_MAX là an toàn).
 */
static void mgt_endpoint(char *buf, size_t bufsz, const char *path) {
    snprintf(buf, bufsz, "%s%s", mgt_base_url(), path);
}

/*
 * json_extract_string — Lấy giá trị string của field "key" trong JSON body.
 *
 * Parser tối giản: tìm "key", bỏ qua khoảng trắng và dấu ':',
 * sau đó copy chuỗi nằm giữa cặp dấu ngoặc kép (xử lý \" và \\).
 * Trả về malloc'd string (caller free), hoặc NULL nếu không tìm thấy.
 */
static char *json_extract_string(const char *json, const char *key) {
    if (!json || !key) return NULL;

    /* Tìm "key" — cần khớp đúng pattern "<key>" để tránh nhầm với prefix khác */
    size_t klen = strlen(key);
    char  *needle = malloc(klen + 4);
    if (!needle) return NULL;
    snprintf(needle, klen + 4, "\"%s\"", key);

    const char *p = strstr(json, needle);
    free(needle);
    if (!p) return NULL;
    p += klen + 2;  /* sau "key" */

    /* Bỏ qua whitespace, ':' */
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;

    /* Tìm dấu " kết thúc, xử lý escape */
    const char *start = p;
    while (*p && *p != '"') {
        if (*p == '\\' && *(p+1)) p += 2;
        else                       p++;
    }
    if (*p != '"') return NULL;

    size_t len = (size_t)(p - start);
    char  *out = malloc(len + 1);
    if (!out) return NULL;
    /* Copy nguyên si — JWT không chứa escape thật, đủ cho use case này */
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/* ─── NE list từ mgt-svc ──────────────────────────────────────── */

/* Thông tin một Network Element lấy từ API /aa/list/ne */
typedef struct ne_item {
    char site[64];
    char ne[64];
    char ip[64];
    char description[128];
    char ns[64];              /* namespace */
    int  port;
    char conf_master_ip[64];  /* IP master ConfD MAAPI */
    int  conf_port_master_tcp;/* Port ConfD IPC (MAAPI) master */
    char conf_slave_ip[64];   /* IP slave — fallback khi master không kết nối được */
    int  conf_port_slave_tcp; /* Port ConfD IPC slave */
} ne_item_t;

#define NE_LIST_MAX 128

/*
 * json_extract_int — Lấy giá trị integer của field "key" trong JSON object.
 * Trả về giá trị int, hoặc def nếu không tìm thấy.
 */
static int json_extract_int(const char *json, const char *key, int def) {
    if (!json || !key) return def;

    size_t klen = strlen(key);
    char *needle = malloc(klen + 4);
    if (!needle) return def;
    snprintf(needle, klen + 4, "\"%s\"", key);

    const char *p = strstr(json, needle);
    free(needle);
    if (!p) return def;
    p += klen + 2;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return def;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    if (*p == '-' || (*p >= '0' && *p <= '9'))
        return atoi(p);
    return def;
}

/* Comparator cho qsort: sort NE list theo (site, ne) — case-insensitive trên site. */
static int ne_cmp_by_site(const void *a, const void *b) {
    const ne_item_t *x = (const ne_item_t *)a;
    const ne_item_t *y = (const ne_item_t *)b;
    int c = strcasecmp(x->site, y->site);
    if (c != 0) return c;
    return strcasecmp(x->ne, y->ne);
}

/*
 * fetch_ne_list — Gọi GET /aa/list/ne, parse JSON, trả về mảng ne_item_t.
 *
 * @param token  JWT token (header Authorization)
 * @param out    mảng ne_item_t[NE_LIST_MAX] do caller cấp
 * @return       số NE parse được, hoặc -1 nếu lỗi
 */
static int fetch_ne_list(const char *token, ne_item_t *out) {
    char url[1024];
    mgt_endpoint(url, sizeof(url), "/aa/list/ne");

    char auth_hdr[4200];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: %s\r\n", token);

    char *resp = NULL;
    int status = http_get_json(url, auth_hdr, &resp);

    if (status < 0) {
        fprintf(stderr, "%snodes: cannot reach mgt-svc%s (url=%s)\n",
                COLOR_RED, COLOR_RESET, url);
        free(resp); return -1;
    }
    if (status != 200 && status != 302) {
        fprintf(stderr, "%snodes: HTTP %d%s\n", COLOR_RED, status, COLOR_RESET);
        if (resp && *resp) fprintf(stderr, "%s\n", resp);
        free(resp); return -1;
    }
    if (!resp || !*resp) {
        fprintf(stderr, "%snodes: empty response%s\n", COLOR_RED, COLOR_RESET);
        free(resp); return -1;
    }

    const char *arr = strstr(resp, "\"neDataList\"");
    if (!arr) { free(resp); return -1; }

    const char *p = strchr(arr, '[');
    if (!p) { free(resp); return 0; }

    int count = 0;
    p++;
    while (*p && count < NE_LIST_MAX) {
        const char *obj_start = strchr(p, '{');
        if (!obj_start) break;
        const char *obj_end = strchr(obj_start, '}');
        if (!obj_end) break;

        size_t olen = (size_t)(obj_end - obj_start + 1);
        char *obj = malloc(olen + 1);
        if (!obj) break;
        memcpy(obj, obj_start, olen);
        obj[olen] = '\0';

        ne_item_t *ne = &out[count];
        memset(ne, 0, sizeof(*ne));

        char *s;
        if ((s = json_extract_string(obj, "site")))        { snprintf(ne->site, sizeof(ne->site), "%s", s); free(s); }
        if ((s = json_extract_string(obj, "ne")))           { snprintf(ne->ne,   sizeof(ne->ne),   "%s", s); free(s); }
        if ((s = json_extract_string(obj, "ip")))           { snprintf(ne->ip,   sizeof(ne->ip),   "%s", s); free(s); }
        if ((s = json_extract_string(obj, "description")))  { snprintf(ne->description, sizeof(ne->description), "%s", s); free(s); }
        if ((s = json_extract_string(obj, "namespace")))       { snprintf(ne->ns,             sizeof(ne->ns),             "%s", s); free(s); }
        if ((s = json_extract_string(obj, "conf_master_ip"))) { snprintf(ne->conf_master_ip, sizeof(ne->conf_master_ip), "%s", s); free(s); }
        if ((s = json_extract_string(obj, "conf_slave_ip")))  { snprintf(ne->conf_slave_ip,  sizeof(ne->conf_slave_ip),  "%s", s); free(s); }
        ne->port                = json_extract_int(obj, "port", 0);
        ne->conf_port_master_tcp = json_extract_int(obj, "conf_port_master_tcp", 0);
        ne->conf_port_slave_tcp  = json_extract_int(obj, "conf_port_slave_tcp", 0);

        free(obj);
        count++;
        p = obj_end + 1;
    }

    free(resp);

    /* Sort theo site_name để NE cùng site đứng liền nhau, dễ nhìn hơn. */
    if (count > 1) qsort(out, (size_t)count, sizeof(ne_item_t), ne_cmp_by_site);

    return count;
}

/*
 * display_ne_list — Hiển thị bảng NE ra stdout.
 */
static void display_ne_list(const ne_item_t *list, int count) {
    printf("\n%s%-16s  %-4s  %-20s  %-20s  %-15s  %-6s  %s%s\n",
           COLOR_BOLD, "Site", "#", "NE", "Namespace",
           "ConfD IP", "Port", "Description", COLOR_RESET);
    printf("────────────────  ────  ────────────────────  ────────────────────"
           "  ───────────────  ──────  ──────────────────────\n");

    for (int i = 0; i < count; i++) {
        const ne_item_t *ne = &list[i];
        /* Hiển thị conf_master_ip/conf_port_master_tcp — fallback về ip/port nếu trống */
        const char *disp_ip = ne->conf_master_ip[0] ? ne->conf_master_ip : ne->ip;
        int disp_port = ne->conf_port_master_tcp ? ne->conf_port_master_tcp : ne->port;
        printf("%-16s  %-4d  %-20s  %-20s  %-15s  %-6d  %s\n",
               ne->site[0]        ? ne->site        : "-",
               i + 1,
               ne->ne[0]          ? ne->ne          : "-",
               ne->ns[0]          ? ne->ns          : "-",
               disp_ip[0]         ? disp_ip         : "-",
               disp_port,
               ne->description[0] ? ne->description : "-");
    }
    printf("\n");
}

/*
 * select_ne_interactive — Cho user chọn NE bằng số thứ tự hoặc namespace.
 * Lặp lại cho đến khi nhập đúng hoặc Ctrl+C/Ctrl+D để huỷ.
 *
 * @return  index (0-based) trong mảng, hoặc -1 nếu huỷ
 */
static int select_ne_interactive(const ne_item_t *list, int count) {
    while (1) {
        char *input = readline("Select NE (# or namespace): ");
        if (!input) return -1;  /* EOF / Ctrl+D */

        char *trimmed = str_trim(input);
        if (!*trimmed) { free(input); continue; }

        /* User gõ "exit" / "quit" ở màn chọn NE → thoát hẳn program. Đặt flag
         * để tầng ngoài biết đây là quit chủ động, phân biệt với cancel (-1). */
        if (strcasecmp(trimmed, "exit") == 0 ||
            strcasecmp(trimmed, "quit") == 0) {
            g_quit_requested = 1;
            free(input);
            return -1;
        }

        /* Thử parse số */
        char *endp;
        long idx = strtol(trimmed, &endp, 10);
        if (*endp == '\0' && idx >= 1 && idx <= count) {
            free(input);
            return (int)(idx - 1);
        }

        /* So sánh namespace (case-insensitive) */
        for (int i = 0; i < count; i++) {
            if (strcasecmp(trimmed, list[i].ns) == 0) {
                free(input);
                return i;
            }
        }

        printf("%sInvalid selection '%s'.%s Enter 1-%d or a namespace, or 'exit' to quit.\n",
               COLOR_RED, trimmed, COLOR_RESET, count);
        free(input);
    }
}

/*
 * cmd_nodes — Lấy và hiển thị danh sách Network Element từ mgt-svc.
 * Yêu cầu: phải login trước (cần JWT token).
 */
static void cmd_nodes(void) {
    const char *token = *g_mgt_token ? g_mgt_token : getenv("MGT_SVC_TOKEN");
    if (!token || !*token) {
        fprintf(stderr, "%sPlease login first.%s  (login <username>)\n",
                COLOR_RED, COLOR_RESET);
        return;
    }

    ne_item_t nes[NE_LIST_MAX];
    int n = fetch_ne_list(token, nes);
    if (n <= 0) {
        if (n == 0) printf("No network elements found.\n");
        return;
    }
    display_ne_list(nes, n);
}

/*
 * read_password — Đọc password từ stdin với echo tắt (giống ssh prompt).
 * Trả về malloc'd string (caller free), hoặc NULL nếu lỗi/EOF.
 */
static char *read_password(const char *prompt) {
    fputs(prompt, stdout); fflush(stdout);

    struct termios old_tio, new_tio;
    int raw_ok = (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &old_tio) == 0);
    if (raw_ok) {
        new_tio = old_tio;
        new_tio.c_lflag &= ~ECHO;   /* tắt echo, giữ ICANON để Enter vẫn submit */
        tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
    }

    char  *buf = NULL;
    size_t cap = 0;
    ssize_t got = getline(&buf, &cap, stdin);

    if (raw_ok) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
        fputc('\n', stdout);
    }

    if (got < 0) { free(buf); return NULL; }
    /* Trim trailing newline */
    if (got > 0 && buf[got-1] == '\n') buf[got-1] = '\0';
    return buf;
}

/*
 * cmd_login - Đăng nhập mgt-svc và lấy JWT token.
 *
 *   login <username>             Hỏi password (echo tắt)
 *   login <username> <password>  Truyền thẳng (lưu ý: lưu vào history)
 *
 * POST {MGT_SVC_BASE}/aa/authenticate với body {username, password}.
 * Response 200 + {"response_data": "<JWT>"} → lưu vào g_mgt_token.
 */
static void cmd_login(char **args, int argc) {
    if (argc < 1) {
        printf("Usage: login <username> [<password>]\n");
        return;
    }

    const char *user = args[0];
    char *password_in = NULL;
    const char *pwd;

    if (argc >= 2) {
        pwd = args[1];
    } else {
        password_in = read_password("Password: ");
        if (!password_in) {
            fprintf(stderr, "%slogin aborted%s\n", COLOR_RED, COLOR_RESET);
            return;
        }
        pwd = password_in;
    }

    char *u_esc = json_escape(user);
    char *p_esc = json_escape(pwd);
    if (password_in) {
        /* Wipe + free password buffer ngay khi không cần */
        memset(password_in, 0, strlen(password_in));
        free(password_in);
    }
    if (!u_esc || !p_esc) {
        free(u_esc); free(p_esc);
        fprintf(stderr, "%slogin: out of memory%s\n", COLOR_RED, COLOR_RESET);
        return;
    }

    size_t blen = strlen(u_esc) + strlen(p_esc) + 64;
    char *body = malloc(blen);
    if (!body) {
        free(u_esc); free(p_esc); return;
    }
    snprintf(body, blen,
             "{\"username\":\"%s\",\"password\":\"%s\"}", u_esc, p_esc);
    free(u_esc);
    /* p_esc chứa password — wipe trước khi free */
    memset(p_esc, 0, strlen(p_esc));
    free(p_esc);

    char url[1024];
    mgt_endpoint(url, sizeof(url), "/aa/authenticate");

    char *resp = NULL;
    int status = http_post_json(url, body, NULL, &resp);
    /* Wipe body (chứa password) */
    memset(body, 0, blen);
    free(body);

    if (status < 0) {
        fprintf(stderr, "%slogin: cannot reach mgt-svc%s (url=%s)\n",
                COLOR_RED, COLOR_RESET, url);
        free(resp); return;
    }
    if (status != 200) {
        printf("%sLogin failed%s — HTTP %d\n", COLOR_RED, COLOR_RESET, status);
        if (resp && *resp) fprintf(stderr, "%s\n", resp);
        free(resp); return;
    }

    /* Parse JWT + status từ response */
    char *token = json_extract_string(resp ? resp : "", "response_data");
    char *st    = json_extract_string(resp ? resp : "", "status");
    free(resp);

    /* Check status field trước — mgt-svc trả 200 cả khi sai password,
     * phân biệt qua "status":"failure" */
    if (st && strcasecmp(st, "success") != 0) {
        fprintf(stderr, "%sLogin failed%s — status=%s\n",
                COLOR_RED, COLOR_RESET, st);
        free(token); free(st);
        return;
    }
    if (!token || !*token) {
        fprintf(stderr, "%slogin: no response_data (JWT) in response%s\n",
                COLOR_RED, COLOR_RESET);
        free(token); free(st);
        return;
    }

    /* Lưu token + username vào globals */
    snprintf(g_mgt_token, sizeof(g_mgt_token), "%s", token);
    snprintf(g_mgt_user,  sizeof(g_mgt_user),  "%s", user);
    free(token); free(st);

    LOG_INFO("runtime login success: user=%s", user);
    printf("%sLogged in%s as %s%s%s\n",
           COLOR_GREEN, COLOR_RESET,
           COLOR_CYAN, user, COLOR_RESET);

    /* Sau login thành công → chọn NE */
    select_and_connect_ne();
}

/*
 * select_and_connect_ne — Lấy danh sách NE, cho user chọn, connect MAAPI.
 *
 * Nếu connect thất bại sẽ cho chọn lại NE (không cần re-login).
 * Trả về 0 nếu connect thành công, -1 nếu user huỷ hoặc lỗi không phục hồi.
 */
static int select_and_connect_ne(void) {
    LOG_INFO("fetching NE list for user=%s", g_mgt_user);
    printf("\nFetching NE list...\n");
    ne_item_t nes[NE_LIST_MAX];
    int ne_count = fetch_ne_list(g_mgt_token, nes);
    if (ne_count < 0) {
        LOG_ERROR("fetch NE list failed for user=%s", g_mgt_user);
        fprintf(stderr, "%sCould not fetch NE list.%s\n",
                COLOR_RED, COLOR_RESET);
        return -1;
    }
    if (ne_count == 0) {
        LOG_WARN("no NEs assigned to user=%s", g_mgt_user);
        printf("No NEs assigned to this user.\n");
        return -1;
    }
    LOG_INFO("NE list: %d NEs available for user=%s", ne_count, g_mgt_user);

    display_ne_list(nes, ne_count);

    /* Loop chọn NE: nếu connect thất bại → hiển thị lại danh sách cho chọn lại */
    while (1) {
        int sel = select_ne_interactive(nes, ne_count);
        if (sel < 0) {
            if (!g_quit_requested) printf("NE selection cancelled.\n");
            return -1;
        }

        const ne_item_t *chosen = &nes[sel];

        /* Dùng conf_master_ip / conf_port_master_tcp để kết nối ConfD MAAPI,
         * fallback về ip/port nếu trường conf_* trống */
        const char *conn_ip   = chosen->conf_master_ip[0] ? chosen->conf_master_ip : chosen->ip;
        int         conn_port = chosen->conf_port_master_tcp ? chosen->conf_port_master_tcp : chosen->port;

        printf("\nConnecting to %s%s%s (%s:%d)...\n",
               COLOR_CYAN, chosen->ne, COLOR_RESET,
               conn_ip, conn_port);

        /* Đóng MAAPI session cũ */
        if (g_maapi) {
            cli_session_close(g_maapi);
            g_maapi = NULL;
        }
        if (g_schema) {
            schema_free(g_schema);
            g_schema = NULL;
        }

        /* Kết nối MAAPI mới tới NE đã chọn */
        const char *maapi_user = env_or("MAAPI_USER", "admin");
        if (*g_rhost)
            LOG_INFO("NE selected: user=%s rhost=%s NE=%s (%s:%d)",
                     g_mgt_user, g_rhost, chosen->ne, conn_ip, conn_port);
        else
            LOG_INFO("NE selected: user=%s NE=%s (%s:%d)",
                     g_mgt_user, chosen->ne, conn_ip, conn_port);
        g_maapi = maapi_dial(conn_ip, conn_port, maapi_user);

        /* Master failed → thử slave nếu mgt-svc có trả về thông tin slave. */
        if (!g_maapi && chosen->conf_slave_ip[0] && chosen->conf_port_slave_tcp) {
            const char *slave_ip   = chosen->conf_slave_ip;
            int         slave_port = chosen->conf_port_slave_tcp;
            LOG_WARN("master %s:%d unreachable, trying slave %s:%d (NE=%s)",
                     conn_ip, conn_port, slave_ip, slave_port, chosen->ne);
            fprintf(stderr, "%sMaster %s:%d unreachable, trying slave %s:%d...%s\n",
                    COLOR_YELLOW, conn_ip, conn_port, slave_ip, slave_port, COLOR_RESET);
            g_maapi = maapi_dial(slave_ip, slave_port, maapi_user);
            if (g_maapi) {
                conn_ip   = slave_ip;
                conn_port = slave_port;
                LOG_INFO("connected to slave NE=%s %s:%d", chosen->ne, conn_ip, conn_port);
            }
        }

        if (!g_maapi) {
            LOG_ERROR("MAAPI connect failed: NE=%s master=%s:%d slave=%s:%d",
                      chosen->ne, conn_ip, conn_port,
                      chosen->conf_slave_ip[0] ? chosen->conf_slave_ip : "-",
                      chosen->conf_port_slave_tcp);
            if (chosen->conf_slave_ip[0]) {
                fprintf(stderr, "%sMAAPI connect failed: master %s:%d and slave %s:%d both unreachable.%s\n",
                        COLOR_RED, conn_ip, conn_port,
                        chosen->conf_slave_ip, chosen->conf_port_slave_tcp, COLOR_RESET);
            } else {
                fprintf(stderr, "%sMAAPI connect to %s:%d failed. Please select another NE.%s\n",
                        COLOR_RED, conn_ip, conn_port, COLOR_RESET);
            }
            display_ne_list(nes, ne_count);
            continue;  /* Cho chọn lại */
        }

        /* Cập nhật NE name + reload schema */
        snprintf(g_ne_name, sizeof(g_ne_name), "%s", chosen->ne);
        update_prompt();

        printf("Loading schema...\n");
        g_schema = schema_new_node("__root__");
        if (g_schema) {
            maapi_load_schema_into(g_maapi, &g_schema);
            printf("Schema loaded.\n");
        }

        LOG_INFO("connected to NE=%s ns=%s (%s:%d) user=%s",
                 chosen->ne, chosen->ns, conn_ip, conn_port, g_mgt_user);
        printf("%sConnected to %s%s (%s:%d)%s\n",
               COLOR_GREEN, chosen->ne,
               chosen->ns[0] ? " " : "", chosen->ns,
               conn_port, COLOR_RESET);
        return 0;
    }
}

/*
 * cmd_logout - Xoá JWT token khỏi bộ nhớ.
 */
static void cmd_logout(void) {
    if (!*g_mgt_token) {
        printf("Not logged in.\n");
        return;
    }
    LOG_INFO("logout: user=%s", g_mgt_user);
    /* Wipe token để không bị peek qua /proc/<pid>/mem */
    memset(g_mgt_token, 0, sizeof(g_mgt_token));
    g_mgt_user[0] = '\0';
    printf("%sLogged out.%s\n", COLOR_GREEN, COLOR_RESET);
}

/*
 * cmd_save - POST CLI operation history lên mgt-svc.
 *
 * API: POST /aa/history/save (mgt-svc swagger)
 *   Headers: Authorization: <token>, Content-Type: application/json
 *            (token đã chứa sẵn prefix "Basic " từ response_data)
 *   Body:    { cmd_name, ne_name, ne_ip, scope?, result? }
 *
 * Cú pháp:
 *   save [--scope=X] [--result=Y] [--ne=NAME] [--ip=IP] <cmd_name...>
 *
 * Mặc định lấy từ:
 *   ne_name  ← env NE_NAME (g_ne_name)
 *   ne_ip    ← env NE_IP, fallback ConfD host
 *   scope    ← "ne-config" (để mgt-svc filter riêng history config NE)
 *   result   ← "success"
 *
 * Env vars:
 *   MGT_SVC_URL    URL đầy đủ (mặc định: http://127.0.0.1:8080/aa/history/save)
 *   MGT_SVC_TOKEN  token cho header "Authorization" (đã có prefix "Basic ")
 */
static void cmd_save(char **args, int argc) {
    const char *scope  = "ne-config";
    const char *result = "success";
    const char *ne     = g_ne_name;
    const char *ne_ip  = env_or("NE_IP", g_maapi ? g_maapi->host : "");

    /* Parse các flag --key=value, phần còn lại ghép thành cmd_name */
    char *cmd_parts[64];
    int   cmd_cnt = 0;
    for (int i = 0; i < argc; i++) {
        if (strncmp(args[i], "--scope=", 8) == 0)       scope  = args[i] + 8;
        else if (strncmp(args[i], "--result=", 9) == 0) result = args[i] + 9;
        else if (strncmp(args[i], "--ne=", 5) == 0)     ne     = args[i] + 5;
        else if (strncmp(args[i], "--ip=", 5) == 0)     ne_ip  = args[i] + 5;
        else if (cmd_cnt < 64) cmd_parts[cmd_cnt++] = args[i];
    }

    if (cmd_cnt == 0) {
        printf("Usage: save [--scope=X] [--result=Y] [--ne=NAME] [--ip=IP] "
               "<cmd_name...>\n"
               "  scope:  cli-config | ne-command | ne-config\n"
               "  result: success    | failure\n");
        return;
    }

    /* Ghép các phần command */
    size_t total = 1;
    for (int i = 0; i < cmd_cnt; i++) total += strlen(cmd_parts[i]) + 1;
    char *cmd = malloc(total);
    if (!cmd) return;
    cmd[0] = '\0';
    for (int i = 0; i < cmd_cnt; i++) {
        if (i) strcat(cmd, " ");
        strcat(cmd, cmd_parts[i]);
    }

    /* Escape tất cả field cho JSON */
    char *cmd_esc    = json_escape(cmd);
    char *ne_esc     = json_escape(ne);
    char *ip_esc     = json_escape(ne_ip);
    char *scope_esc  = json_escape(scope);
    char *result_esc = json_escape(result);
    free(cmd);
    if (!cmd_esc || !ne_esc || !ip_esc || !scope_esc || !result_esc) {
        free(cmd_esc); free(ne_esc); free(ip_esc);
        free(scope_esc); free(result_esc);
        return;
    }

    /* Build body theo swagger: cmd_name, ne_name, ne_ip, scope, result */
    size_t blen = strlen(cmd_esc) + strlen(ne_esc) + strlen(ip_esc)
                + strlen(scope_esc) + strlen(result_esc) + 256;
    char *body = malloc(blen);
    if (!body) {
        free(cmd_esc); free(ne_esc); free(ip_esc);
        free(scope_esc); free(result_esc);
        return;
    }
    snprintf(body, blen,
             "{\"cmd_name\":\"%s\","
             "\"ne_name\":\"%s\","
             "\"ne_ip\":\"%s\","
             "\"scope\":\"%s\","
             "\"result\":\"%s\"}",
             cmd_esc, ne_esc, ip_esc, scope_esc, result_esc);
    free(cmd_esc); free(ne_esc); free(ip_esc);
    free(scope_esc); free(result_esc);

    /* Build Authorization header — ưu tiên token in-memory từ login,
     * fallback về env MGT_SVC_TOKEN (cho automation/CI).
     * Token đã có prefix "Basic " từ response_data, dùng nguyên. */
    char auth_hdr[8192] = "";
    const char *token = (*g_mgt_token) ? g_mgt_token : getenv("MGT_SVC_TOKEN");
    if (token && *token) {
        snprintf(auth_hdr, sizeof(auth_hdr),
                 "Authorization: %s\r\n", token);
    } else {
        fprintf(stderr, "%sNot logged in%s — run %slogin <user>%s first.\n",
                COLOR_YELLOW, COLOR_RESET, COLOR_CYAN, COLOR_RESET);
    }

    /* URL: ưu tiên override MGT_SVC_URL, không thì base + path mặc định */
    char url_buf[1024];
    const char *url;
    const char *override = getenv("MGT_SVC_URL");
    if (override && *override) {
        url = override;
    } else {
        mgt_endpoint(url_buf, sizeof(url_buf), "/aa/history/save");
        url = url_buf;
    }

    char *resp = NULL;
    int status = http_post_json(url, body,
                                auth_hdr[0] ? auth_hdr : NULL, &resp);
    free(body);

    const char *who = (*g_mgt_user) ? g_mgt_user : g_cli_user;
    if (status < 0) {
        LOG_WARN("save FAILED (mgt-svc unreachable): user=%s ne=%s scope=%s",
                 who, ne, scope);
        fprintf(stderr, "%smgt-svc POST failed%s (url=%s)\n",
                COLOR_RED, COLOR_RESET, url);
    } else if (status == 201) {
        LOG_INFO("save OK: user=%s ne=%s scope=%s result=%s",
                 who, ne, scope, result);
        printf("%sSaved%s (HTTP 201) → %s\n",
               COLOR_GREEN, COLOR_RESET, url);
        if (resp && *resp) printf("%s\n", resp);
    } else {
        LOG_WARN("save FAILED: user=%s ne=%s scope=%s http=%d",
                 who, ne, scope, status);
        printf("%smgt-svc returned HTTP %d%s\n",
               COLOR_RED, status, COLOR_RESET);
        if (resp && *resp) fprintf(stderr, "%s\n", resp);
    }
    free(resp);
}

/**
 * cmd_rollback - Liệt kê hoặc áp dụng rollback file của ConfD.
 *
 *   rollback                      Liệt kê các rollback hiện có.
 *   rollback <nr>                 Stage rollback số <nr> vào candidate (cần `commit`).
 *   rollback <nr> commit          Stage và commit luôn.
 *
 * @param args  Mảng tham số
 * @param argc  Số lượng tham số
 */
static void cmd_rollback(char **args, int argc) {
    REQUIRE_MAAPI();
    if (argc == 0) {
        struct rollback_entry list[64];
        int n = maapi_do_list_rollbacks(g_maapi, list, 64);
        if (n < 0) {
            fprintf(stderr, "%srollback list failed%s\n", COLOR_RED, COLOR_RESET);
            return;
        }
        if (n == 0) {
            printf("No rollback files available.\n");
            return;
        }
        printf("%-6s %-20s %-12s %s\n", "NR", "DATE", "VIA", "CREATOR");
        for (int i = 0; i < n; i++) {
            printf("%-6d %-20s %-12s %s%s%s\n",
                   list[i].nr, list[i].datestr, list[i].via,
                   COLOR_CYAN, list[i].creator, COLOR_RESET);
            if (list[i].label[0] || list[i].comment[0])
                printf("       label=%s  comment=%s\n",
                       list[i].label, list[i].comment);
        }
        return;
    }

    /* Parse số rollback */
    char *end = NULL;
    long nr = strtol(args[0], &end, 10);
    if (end == args[0] || *end != '\0') {
        fprintf(stderr, "Usage: rollback [<nr> [commit]]\n");
        return;
    }

    if (maapi_do_load_rollback(g_maapi, (int)nr) != 0) {
        fprintf(stderr, "%srollback %ld failed%s\n", COLOR_RED, nr, COLOR_RESET);
        return;
    }
    printf("%sRollback %ld staged%s in candidate.\n",
           COLOR_GREEN, nr, COLOR_RESET);

    /* Tuỳ chọn commit ngay */
    if (argc >= 2 && strcasecmp(args[1], "commit") == 0) {
        cmd_commit();
    } else {
        printf("Run %scommit%s to apply, or %sdiscard%s to abort.\n",
               COLOR_CYAN, COLOR_RESET, COLOR_CYAN, COLOR_RESET);
    }
}

/*
 * cmd_bench - Hidden benchmark command.
 *
 * Usage:
 *   bench <path...> <N> [key_prefix]
 *
 * Walk schema theo <path...>. Path phải kết thúc ở 1 YANG list có n_keys == 1.
 * Sinh N list entry với key = "<prefix><idx>" (prefix mặc định "bench_"),
 * nạp candidate qua maapi_load_xml, validate, commit. In thời gian từng pha.
 *
 * Các entry chỉ có key leaf — không set thêm leaf khác. Đủ để đo đường load +
 * commit cho danh sách N entry.
 */
static void cmd_bench(char **args, int argc) {
    REQUIRE_MAAPI();
    if (argc < 2) {
        fprintf(stderr,
            "%sUsage:%s bench <path...> <N> [key_prefix]\n"
            "  eg: bench system ntp server 1000\n"
            "      bench system ntp server 1000 node_\n",
            COLOR_CYAN, COLOR_RESET);
        return;
    }
    if (!g_schema) {
        fprintf(stderr, "%sNo schema loaded.%s\n", COLOR_RED, COLOR_RESET);
        return;
    }

    /* Tách N và prefix từ đuôi args.
     * N luôn là phần tử cuối cùng hoặc áp chót (nếu có prefix). */
    int path_argc = argc;
    const char *key_prefix = "bench_";
    char *endp = NULL;
    long n_last = strtol(args[argc - 1], &endp, 10);
    if (endp && *endp == '\0' && n_last > 0) {
        /* Arg cuối là N */
        path_argc = argc - 1;
    } else if (argc >= 3) {
        /* Arg cuối có thể là key_prefix, áp chót là N */
        endp = NULL;
        long n_prev = strtol(args[argc - 2], &endp, 10);
        if (endp && *endp == '\0' && n_prev > 0) {
            n_last = n_prev;
            key_prefix = args[argc - 1];
            path_argc = argc - 2;
        } else {
            fprintf(stderr, "%sInvalid N — expected positive integer.%s\n",
                    COLOR_RED, COLOR_RESET);
            return;
        }
    } else {
        fprintf(stderr, "%sInvalid N — expected positive integer.%s\n",
                COLOR_RED, COLOR_RESET);
        return;
    }
    int N = (int)n_last;
    if (path_argc < 1) {
        fprintf(stderr, "%sMissing path to list.%s\n", COLOR_RED, COLOR_RESET);
        return;
    }

    /* Walk schema theo path_argc token, ghi nhận node + xmlns của token đầu. */
    schema_node_t *chain[MAX_PATH_DEPTH];
    int chain_len = 0;
    schema_node_t *cur = g_schema;
    for (int i = 0; i < path_argc; i++) {
        schema_node_t *found = NULL;
        for (schema_node_t *c = cur->children; c; c = c->next) {
            if (strcasecmp(c->name, args[i]) == 0) { found = c; break; }
        }
        if (!found) {
            fprintf(stderr, "%sUnknown node '%s'%s at depth %d\n",
                    COLOR_RED, args[i], COLOR_RESET, i);
            return;
        }
        if (chain_len >= MAX_PATH_DEPTH) {
            fprintf(stderr, "%sPath too deep.%s\n", COLOR_RED, COLOR_RESET);
            return;
        }
        chain[chain_len++] = found;
        cur = found;
    }

    schema_node_t *list_node = chain[chain_len - 1];
    if (!list_node->is_list) {
        fprintf(stderr, "%sLast path token '%s' is not a YANG list.%s\n",
                COLOR_RED, list_node->name, COLOR_RESET);
        return;
    }
    if (list_node->n_keys != 1) {
        fprintf(stderr, "%sOnly single-key lists supported by bench (got n_keys=%d).%s\n",
                COLOR_RED, list_node->n_keys, COLOR_RESET);
        return;
    }
    const char *key_name = list_node->keys[0];

    printf("%sBench:%s N=%d path=", COLOR_BOLD, COLOR_RESET, N);
    for (int i = 0; i < chain_len; i++) printf("%s%s", i ? "/" : "", chain[i]->name);
    printf(" key=%s prefix=%s\n", key_name, key_prefix);
    fflush(stdout);

    /* ── Pha 1: generate XML ────────────────────────────────── */
    struct timeval tg0; gettimeofday(&tg0, NULL);

    size_t cap = 4096;
    size_t len = 0;
    char *xml = malloc(cap);
    if (!xml) { fprintf(stderr, "oom\n"); return; }
    xml[0] = '\0';

    /* Ước lượng ~70 byte/entry + wrapper. Pre-reserve. */
    size_t want = (size_t)N * 96 + 4096;
    if (want > cap) { cap = want; xml = realloc(xml, cap); if (!xml) { fprintf(stderr, "oom\n"); return; } }

    /* Mở các thẻ ngoài (tất cả trừ cái cuối = list-name). Thẻ đầu gắn xmlns. */
    for (int i = 0; i < chain_len - 1; i++) {
        int need;
        if (i == 0 && chain[i]->ns[0]) {
            need = snprintf(xml + len, cap - len, "<%s xmlns=\"%s\">",
                            chain[i]->name, chain[i]->ns);
        } else {
            need = snprintf(xml + len, cap - len, "<%s>", chain[i]->name);
        }
        if (need < 0) { fprintf(stderr, "snprintf err\n"); free(xml); return; }
        if ((size_t)need >= cap - len) {
            cap = (len + need + 1) * 2;
            xml = realloc(xml, cap);
            if (!xml) { fprintf(stderr, "oom\n"); return; }
            need = snprintf(xml + len, cap - len, "<%s>", chain[i]->name);
        }
        len += need;
    }

    /* Nếu chain chỉ có 1 node (chính là list) và là top-level, cần xmlns trên
     * thẻ list. */
    const char *list_xmlns = (chain_len == 1 && list_node->ns[0]) ? list_node->ns : NULL;

    /* N list entry */
    for (int i = 1; i <= N; i++) {
        /* Grow buffer nếu còn < 256 byte */
        if (cap - len < 256) {
            cap *= 2;
            xml = realloc(xml, cap);
            if (!xml) { fprintf(stderr, "oom\n"); return; }
        }
        int need;
        if (list_xmlns) {
            need = snprintf(xml + len, cap - len,
                            "<%s xmlns=\"%s\"><%s>%s%d</%s></%s>",
                            list_node->name, list_xmlns,
                            key_name, key_prefix, i, key_name,
                            list_node->name);
        } else {
            need = snprintf(xml + len, cap - len,
                            "<%s><%s>%s%d</%s></%s>",
                            list_node->name,
                            key_name, key_prefix, i, key_name,
                            list_node->name);
        }
        if (need < 0 || (size_t)need >= cap - len) {
            cap = (len + 512) * 2;
            xml = realloc(xml, cap);
            if (!xml) { fprintf(stderr, "oom\n"); return; }
            i--;
            continue;
        }
        len += need;
    }

    /* Đóng các thẻ ngoài (ngược thứ tự) */
    for (int i = chain_len - 2; i >= 0; i--) {
        if (cap - len < 256) {
            cap *= 2;
            xml = realloc(xml, cap);
            if (!xml) { fprintf(stderr, "oom\n"); return; }
        }
        int need = snprintf(xml + len, cap - len, "</%s>", chain[i]->name);
        len += need;
    }

    long ms_gen = elapsed_ms(&tg0);
    size_t xml_size = len;

    /* ── Pha 2: load_xml ───────────────────────────────────── */
    struct timeval tl0; gettimeofday(&tl0, NULL);
    int rc_load = maapi_load_xml(g_maapi, xml);
    long ms_load = elapsed_ms(&tl0);
    free(xml);

    if (rc_load != 0) {
        fprintf(stderr, "%sload_xml failed%s\n", COLOR_RED, COLOR_RESET);
        LOG_WARN("bench load_xml FAILED: user=%s ne=%s N=%d",
                 (*g_mgt_user) ? g_mgt_user : g_cli_user, g_ne_name, N);
        return;
    }

    /* ── Pha 3: validate ───────────────────────────────────── */
    struct timeval tv0; gettimeofday(&tv0, NULL);
    int rc_val = maapi_do_validate(g_maapi);
    long ms_val = elapsed_ms(&tv0);

    if (rc_val != 0) {
        fprintf(stderr, "%svalidate failed%s\n", COLOR_RED, COLOR_RESET);
        LOG_WARN("bench validate FAILED: user=%s ne=%s N=%d",
                 (*g_mgt_user) ? g_mgt_user : g_cli_user, g_ne_name, N);
        return;
    }

    /* ── Pha 4: commit ─────────────────────────────────────── */
    struct timeval tc0; gettimeofday(&tc0, NULL);
    int rc_commit = maapi_do_commit(g_maapi);
    long ms_commit = elapsed_ms(&tc0);

    if (rc_commit != 0) {
        fprintf(stderr, "%scommit failed%s\n", COLOR_RED, COLOR_RESET);
        LOG_WARN("bench commit FAILED: user=%s ne=%s N=%d",
                 (*g_mgt_user) ? g_mgt_user : g_cli_user, g_ne_name, N);
        return;
    }

    long ms_total = ms_gen + ms_load + ms_val + ms_commit;
    invalidate_xml_cache();

    printf("\n%sBench results (N=%d, XML=%zu bytes):%s\n",
           COLOR_BOLD, N, xml_size, COLOR_RESET);
    printf("  %-12s %8ld ms\n", "generate",  ms_gen);
    printf("  %-12s %8ld ms  (%.2f ms/entry)\n",
           "load_xml", ms_load, N > 0 ? (double)ms_load / N : 0.0);
    printf("  %-12s %8ld ms\n", "validate",  ms_val);
    printf("  %-12s %8ld ms  (%.2f ms/entry)\n",
           "commit",   ms_commit, N > 0 ? (double)ms_commit / N : 0.0);
    printf("  %-12s %8ld ms  (%.2f ms/entry)\n",
           "TOTAL",    ms_total, N > 0 ? (double)ms_total / N : 0.0);
    LOG_INFO("bench OK: user=%s ne=%s N=%d gen=%ld load=%ld val=%ld commit=%ld",
             (*g_mgt_user) ? g_mgt_user : g_cli_user, g_ne_name,
             N, ms_gen, ms_load, ms_val, ms_commit);
}

/*
 * cmd_help - Hiển thị bảng trợ giúp với danh sách lệnh và ví dụ sử dụng.
 */
static void cmd_help(void) {
    printf(
        "\n%sMAAPI Direct Mode Commands%s\n"
        "──────────────────────────────────────────────────────────\n"
        "  show running-config [path...]     Get running config\n"
        "  show candidate-config [path...]   Get candidate config\n"
        "\n"
        "  set <path...> <value>             Set leaf (space-separated)\n"
        "  set                               Paste XML config block\n"
        "\n"
        "  unset <path...>                   Delete node (space-separated)\n"
        "\n"
        "  commit                            Commit candidate → running\n"
        "  validate                          Validate candidate\n"
        "  discard                           Reset candidate to running\n"
        "  lock [running|candidate]          Lock datastore\n"
        "  unlock [running|candidate]        Unlock datastore\n"
        "  dump text [file]                  Export config as text\n"
        "  dump xml  [file]                  Export config as XML\n"
        "\n"
        "  rollback                          List rollback files\n"
        "  rollback <nr> [commit]            Stage rollback (commit = áp dụng luôn)\n"
        "\n"
        "  login <user> [<password>]         Đăng nhập mgt-svc, lấy JWT token\n"
        "  logout                            Xoá token khỏi bộ nhớ\n"
        "  save [--scope=X --result=Y] <cmd_name>\n"
        "                                    POST CLI op history → mgt-svc\n"
        "                                    (env MGT_SVC_BASE, MGT_SVC_URL, MGT_SVC_TOKEN)\n"
        "  nodes                             List NEs from mgt-svc (requires login)\n"
        "  help                              This message\n"
        "  exit                              Quit\n"
        "\n"
        "%sPath examples (space-separated, giống Go):%s\n"
        "  set system hostname new-host\n"
        "  set system ntp enabled true\n"
        "  set system ntp server 10.0.0.1 prefer true    ← key list\n"
        "  unset system ntp server 10.0.0.1\n"
        "  show running-config system ntp\n"
        "\n"
        "%sKeypath ConfD cũng được chấp nhận:%s\n"
        "  set /system/hostname new-host\n"
        "  unset /system/ntp/server{10.0.0.1}\n"
        "\n"
        "  %sTab completion%s: commands + top-level schema nodes.\n\n",
        COLOR_BOLD, COLOR_RESET,
        COLOR_CYAN, COLOR_RESET,
        COLOR_CYAN, COLOR_RESET,
        COLOR_CYAN, COLOR_RESET);
}

/* ─── Phân phối lệnh (Dispatch) ─────────────────────────────── */

/**
 * dispatch - Phân tích dòng lệnh và chuyển tới hàm xử lý tương ứng.
 *
 * Tách dòng lệnh thành các token, so sánh token đầu tiên với tên lệnh
 * đã biết, rồi gọi hàm cmd_* tương ứng. Nếu lệnh không nhận dạng được,
 * hiển thị thông báo lỗi.
 *
 * @param line  Chuỗi dòng lệnh đã được trim (không có khoảng trắng đầu/cuối)
 */
static void dispatch(char *line) {
    int    argc = 0;
    char **argv = str_split(line, &argc);
    if (!argc) { free_tokens(argv, argc); return; }

    {
        const char *who = (*g_mgt_user) ? g_mgt_user : g_cli_user;
        if (*g_rhost)
            LOG_INFO("cmd: user=%s rhost=%s ne=%s >> %s",
                     who, g_rhost, g_ne_name, line);
        else
            LOG_INFO("cmd: user=%s ne=%s >> %s", who, g_ne_name, line);
    }

    /* Lệnh thay đổi candidate → invalidate cache XML của tab-completion để
     * vòng TAB kế tiếp nhìn thấy state mới. */
    if (strcasecmp(argv[0], "set")      == 0 ||
        strcasecmp(argv[0], "unset")    == 0 ||
        strcasecmp(argv[0], "commit")   == 0 ||
        strcasecmp(argv[0], "discard")  == 0 ||
        strcasecmp(argv[0], "rollback") == 0 ||
        strcasecmp(argv[0], "bench")    == 0) {
        invalidate_xml_cache();
    }

    /* Bảng phân phối lệnh: so sánh không phân biệt hoa/thường */
    if      (strcasecmp(argv[0], "show")     == 0) cmd_show    (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "set")      == 0) cmd_set     (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "unset")    == 0) cmd_unset   (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "commit")   == 0) cmd_commit  ();
    else if (strcasecmp(argv[0], "validate") == 0) cmd_validate();
    else if (strcasecmp(argv[0], "discard")  == 0) cmd_discard ();
    else if (strcasecmp(argv[0], "lock")     == 0) cmd_lock    (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "unlock")   == 0) cmd_unlock  (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "dump")     == 0) cmd_dump    (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "rollback") == 0) cmd_rollback(argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "login")    == 0) cmd_login   (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "logout")   == 0) cmd_logout  ();
    else if (strcasecmp(argv[0], "save")     == 0) cmd_save    (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "nodes")    == 0) cmd_nodes   ();
    else if (strcasecmp(argv[0], "bench")    == 0) cmd_bench   (argv + 1, argc - 1);
    else if (strcasecmp(argv[0], "help")     == 0) cmd_help    ();
    else if (strcasecmp(argv[0], "?")        == 0) cmd_help    ();
    else
        printf("%sUnknown command: %s%s  (type 'help')\n",
               COLOR_RED, argv[0], COLOR_RESET);

    free_tokens(argv, argc);
}

/* ─── Hàm chính (Main) ──────────────────────────────────────── */

/**
 * main - Điểm khởi đầu của chương trình CLI NETCONF.
 *
 * Hai chế độ hoạt động:
 *   - Direct mode (debug): set CONFD_IPC_ADDR/CONFD_IPC_PORT → connect thẳng ConfD
 *   - SSH server mode (mặc định): bắt buộc login mgt-svc → chọn NE → connect
 *
 * @return  0 nếu thành công, 1 nếu lỗi không phục hồi
 */
int main(void) {
    /* Cài đặt handler cho SIGINT để Ctrl+C không thoát chương trình */
    signal(SIGINT, sigint_handler);
    /* Bỏ qua SIGPIPE: nếu mgt-svc/peer đóng socket giữa chừng,
     * write() trả về EPIPE thay vì kill app */
    signal(SIGPIPE, SIG_IGN);
    rl_catch_signals = 0;  /* Tắt xử lý tín hiệu mặc định của readline, dùng handler riêng */
    rl_attempted_completion_function = maapi_completer;  /* Đăng ký hàm tab-completion */
    rl_completion_display_matches_hook = path_display_hook;  /* 2-section display (leaf vs key) */
    rl_variable_bind("expand-tilde", "off");  /* Tắt mở rộng dấu ~ trong readline */

    /* Nạp env từ file trước tiên — sshd child không kế thừa env container.
     * setenv(..., 0) nên env truyền qua exec vẫn được ưu tiên. */
    load_env_file("/etc/cli-netconf/env");

    /* Per-user log file nếu chưa set */
    if (!getenv("LOG_FILE")) {
        const char *u = getenv("USER");
        if (!u || !*u) u = getenv("LOGNAME");
        if (u && *u) {
            char log_path[256];
            snprintf(log_path, sizeof(log_path),
                     "/var/log/cli-netconf/%s.log", u);
            setenv("LOG_FILE", log_path, 0);
        }
    }

    log_init();

    /* Lấy username cho prompt: MAAPI_USER (wrapper set) → USER → "admin". */
    {
        const char *u = env_or("MAAPI_USER", NULL);
        if (!u) u = env_or("USER", NULL);
        if (!u) u = env_or("LOGNAME", "admin");
        strncpy(g_cli_user, u, sizeof(g_cli_user) - 1);
        g_cli_user[sizeof(g_cli_user) - 1] = '\0';
    }

    init_rhost();
    if (*g_rhost)
        LOG_INFO("session start: user=%s rhost=%s", g_cli_user, g_rhost);

    int direct_mode = (getenv("CONFD_IPC_ADDR") || getenv("CONFD_IPC_PORT"));
    LOG_INFO("cli-netconf started, mode=%s", direct_mode ? "direct" : "ssh-server");

    /* Hiển thị banner chào mừng */
    printf("%s", COLOR_GREEN);
    printf("=====================================================\n");
    if (direct_mode)
        printf("   CLI - NETCONF Console (Direct Debug Mode)\n");
    else
        printf("   CLI - NETCONF Console\n");
    printf("=====================================================%s\n\n", COLOR_RESET);

    if (direct_mode) {
        /* ── Direct mode: connect thẳng ConfD qua env vars (dùng khi debug) ── */
        const char *host = env_or("CONFD_IPC_ADDR", "127.0.0.1");
        int         port = env_int_or("CONFD_IPC_PORT", CONFD_PORT);
        const char *user = env_or("MAAPI_USER", "admin");
        strncpy(g_ne_name, env_or("NE_NAME", "confd"), sizeof(g_ne_name) - 1);

        printf("Connecting to ConfD MAAPI %s%s:%d%s ...\n",
               COLOR_BOLD, host, port, COLOR_RESET);
        LOG_INFO("direct mode: connecting to %s:%d user=%s", host, port, user);
        g_maapi = maapi_dial(host, port, user);
        if (!g_maapi) {
            LOG_ERROR("direct mode: MAAPI connect to %s:%d failed", host, port);
            fprintf(stderr, "%sMAAPI connect failed. Is ConfD running?%s\n",
                    COLOR_RED, COLOR_RESET);
            log_close();
            return 1;
        }
        LOG_INFO("direct mode: connected to %s:%d", host, port);
        printf("%sConnected.%s\n", COLOR_GREEN, COLOR_RESET);
        printf("Loading schema from MAAPI...\n");
        g_schema = schema_new_node("__root__");
        if (g_schema) {
            maapi_load_schema_into(g_maapi, &g_schema);
            printf("Schema loaded.\n");
        }
    } else {
        /* ── SSH server mode: chọn NE trước khi vào CLI ── */

        /* Kiểm tra token đã có sẵn từ PAM/SSH (env MGT_SVC_TOKEN + MGT_SVC_USER) */
        const char *pre_token = getenv("MGT_SVC_TOKEN");
        const char *pre_user  = getenv("MGT_SVC_USER");
        if (pre_token && *pre_token && pre_user && *pre_user) {
            /* PAM đã authenticate → dùng token có sẵn, nhảy thẳng vào chọn NE */
            snprintf(g_mgt_token, sizeof(g_mgt_token), "%s", pre_token);
            snprintf(g_mgt_user,  sizeof(g_mgt_user),  "%s", pre_user);
            LOG_INFO("pre-authenticated via SSH: user=%s", g_mgt_user);
            printf("Logged in as %s%s%s\n\n",
                   COLOR_CYAN, g_mgt_user, COLOR_RESET);
            fflush(stdout);

            if (select_and_connect_ne() != 0) {
                if (g_quit_requested) {
                    LOG_INFO("exit from NE select: user=%s", g_mgt_user);
                    printf("Goodbye.\n");
                } else {
                    LOG_ERROR("NE selection failed for pre-auth user=%s", g_mgt_user);
                    printf("Cannot connect to any NE.\n");
                }
                log_close();
                return g_quit_requested ? 0 : 1;
            }
            goto cli_ready;
        }

        /* Không có token → hỏi login thủ công (fallback, chạy ngoài SSH) */
        int logged_in = 0;
        while (!logged_in) {
            char *uline = readline("Username: ");
            if (!uline) {
                LOG_INFO("session aborted at username prompt (EOF)");
                printf("\nGoodbye.\n");
                log_close();
                return 0;  /* Ctrl+D → thoát */
            }
            char *username = str_trim(uline);
            if (!*username) { free(uline); continue; }

            char *password = read_password("Password: ");
            if (!password) {
                LOG_INFO("session aborted at password prompt (EOF) user=%s", username);
                free(uline);
                printf("\nGoodbye.\n");
                log_close();
                return 0;
            }

            /* Authenticate với mgt-svc */
            char *u_esc = json_escape(username);
            char *p_esc = json_escape(password);
            memset(password, 0, strlen(password));
            free(password);

            if (!u_esc || !p_esc) {
                free(u_esc); free(p_esc); free(uline);
                fprintf(stderr, "%sOut of memory%s\n", COLOR_RED, COLOR_RESET);
                continue;
            }

            size_t blen = strlen(u_esc) + strlen(p_esc) + 64;
            char *body = malloc(blen);
            if (!body) { free(u_esc); free(p_esc); free(uline); continue; }
            snprintf(body, blen,
                     "{\"username\":\"%s\",\"password\":\"%s\"}", u_esc, p_esc);
            free(u_esc);
            memset(p_esc, 0, strlen(p_esc));
            free(p_esc);

            char url[1024];
            mgt_endpoint(url, sizeof(url), "/aa/authenticate");

            char *resp = NULL;
            int status = http_post_json(url, body, NULL, &resp);
            memset(body, 0, blen);
            free(body);

            LOG_INFO("login attempt: user=%s", username);

            if (status < 0) {
                LOG_ERROR("login: mgt-svc unreachable url=%s", url);
                fprintf(stderr, "%sCannot reach mgt-svc%s (url=%s)\n",
                        COLOR_RED, COLOR_RESET, url);
                free(resp); free(uline);
                continue;
            }
            if (status != 200) {
                LOG_WARN("login failed: user=%s http=%d", username, status);
                printf("%sLogin failed%s — HTTP %d\n", COLOR_RED, COLOR_RESET, status);
                if (resp && *resp) fprintf(stderr, "%s\n", resp);
                free(resp); free(uline);
                continue;
            }

            char *token = json_extract_string(resp ? resp : "", "response_data");
            char *st    = json_extract_string(resp ? resp : "", "status");
            free(resp);

            if (st && strcasecmp(st, "success") != 0) {
                LOG_WARN("login failed: user=%s status=%s", username, st);
                fprintf(stderr, "%sLogin failed%s — %s\n",
                        COLOR_RED, COLOR_RESET, st);
                free(token); free(st); free(uline);
                continue;
            }
            if (!token || !*token) {
                LOG_WARN("login failed: user=%s no token in response", username);
                fprintf(stderr, "%sLogin failed — invalid response%s\n",
                        COLOR_RED, COLOR_RESET);
                free(token); free(st); free(uline);
                continue;
            }

            snprintf(g_mgt_token, sizeof(g_mgt_token), "%s", token);
            snprintf(g_mgt_user,  sizeof(g_mgt_user),  "%s", username);
            free(token); free(st); free(uline);

            LOG_INFO("login success: user=%s", g_mgt_user);
            printf("%sLogged in%s as %s%s%s\n",
                   COLOR_GREEN, COLOR_RESET,
                   COLOR_CYAN, g_mgt_user, COLOR_RESET);

            /* Chọn NE + connect MAAPI */
            if (select_and_connect_ne() == 0) {
                logged_in = 1;
            } else if (g_quit_requested) {
                /* User gõ "exit" ở màn chọn NE → thoát hẳn, không retry login */
                printf("Goodbye.\n");
                log_close();
                return 0;
            } else {
                /* Không chọn được NE → quay lại login */
                memset(g_mgt_token, 0, sizeof(g_mgt_token));
                g_mgt_user[0] = '\0';
                printf("\n%sPlease login again.%s\n\n", COLOR_YELLOW, COLOR_RESET);
            }
        }
    }

cli_ready:
    /* Khởi tạo prompt và lịch sử lệnh readline */
    update_prompt();
    using_history();
    stifle_history(HISTORY_SIZE);  /* Giới hạn số lệnh lưu trong lịch sử */

    printf("\nType %shelp%s for commands. Ctrl+D or %sexit%s to quit.\n\n",
           COLOR_CYAN, COLOR_RESET, COLOR_CYAN, COLOR_RESET);

    /* ── Vòng lặp chính: đọc và xử lý lệnh ──
     * Trong SSH server mode, "exit" KHÔNG thoát chương trình mà quay lại
     * màn hình chọn NE. Chỉ Ctrl+D (readline trả NULL) mới thoát hẳn.
     * Direct mode không có khái niệm NE select → "exit" thoát luôn. */
    char *line;
    while ((line = readline(g_prompt)) != NULL) {
        char *trimmed = str_trim(line);  /* Loại bỏ khoảng trắng đầu/cuối */
        if (*trimmed) {
            add_history(trimmed);  /* Lưu lệnh vào lịch sử (cho phím mũi tên lên/xuống) */
            if (strcasecmp(trimmed, "exit") == 0 ||
                strcasecmp(trimmed, "quit") == 0) {
                free(line);
                if (direct_mode) break;  /* Direct: thoát chương trình */

                /* SSH server mode: đóng MAAPI hiện tại, quay về chọn NE. */
                LOG_INFO("exit from NE=%s → back to NE select", g_ne_name);
                if (g_maapi)  { cli_session_close(g_maapi); g_maapi = NULL; }
                if (g_schema) { schema_free(g_schema);     g_schema = NULL; }
                if (select_and_connect_ne() != 0) {
                    /* User Ctrl+D ở select hoặc không còn NE nào kết nối được → thoát */
                    break;
                }
                update_prompt();
                continue;
            }
            dispatch(trimmed);  /* Phân phối lệnh tới hàm xử lý tương ứng */
        }
        free(line);
        /* Trong dispatch có thể gọi login → select_and_connect_ne → user gõ
         * "exit" ở màn chọn NE. Flag này cho phép thoát chương trình ngay
         * thay vì tiếp tục vào CLI loop với state nửa chừng. */
        if (g_quit_requested) break;
    }

    /* Dọn dẹp tài nguyên trước khi thoát */
    LOG_INFO("session ended: user=%s ne=%s", g_mgt_user, g_ne_name);
    printf("Goodbye.\n");
    if (g_schema)  schema_free(g_schema);
    if (g_maapi)   cli_session_close(g_maapi);
    log_close();
    return 0;
}
