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
#include <sys/ioctl.h>
#include <sys/time.h>
#include "confd_compat.h"
#include <readline/readline.h>
#include <readline/history.h>
#include "cli.h"
#include "maapi-direct.h"

/* ─── Biến toàn cục (Globals) ───────────────────────────────── */

/* Con trỏ tới phiên MAAPI hiện tại, dùng chung cho toàn bộ chương trình */
static maapi_session_t *g_maapi   = NULL;

/* Cây schema YANG đã tải từ ConfD, phục vụ tab-completion và chuyển đổi đường dẫn */
static schema_node_t   *g_schema  = NULL;

/* Chuỗi prompt hiển thị trên dòng lệnh readline */
static char             g_prompt[256];

/* Tên thiết bị mạng (Network Element), hiển thị trong prompt */
static char             g_ne_name[128] = "confd";

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
 * Định dạng: "maapi[<tên_thiết_bị>]> " với màu sắc ANSI.
 */
static void update_prompt(void) {
    snprintf(g_prompt, sizeof(g_prompt),
             "%smaapi%s[%s%s%s]> ",
             COLOR_CYAN, COLOR_RESET,
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
            char ch = (char)getchar();
            printf("\r\033[2K");   /* Xoá dòng prompt <MORE> */
            if (ch == 'q' || ch == 'Q' || ch == 27) return;         /* Thoát pager */
            if (ch == 'a' || ch == 'A' || ch == 'G') { printf("%s", p); return; } /* In hết phần còn lại */
            /* Mặc định (Enter): tiếp tục hiển thị trang tiếp theo */
        }
    }
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
/* Node con đang duyệt (iterator) */
static schema_node_t *g_comp_cur    = NULL;

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

/**
 * path_generator - Hàm sinh gợi ý cho đường dẫn schema YANG.
 *
 * Duyệt qua các node con của node cha (tìm bởi find_completion_parent)
 * và trả về lần lượt các tên node khớp với tiền tố `text`.
 *
 * @param text   Tiền tố mà người dùng đã gõ
 * @param state  0 = lần gọi đầu tiên, != 0 = lần gọi tiếp theo
 * @return       Chuỗi gợi ý (malloc'd) hoặc NULL khi hết
 */
static char *path_generator(const char *text, int state) {
    if (!state) {
        /* Lần gọi đầu: xác định node cha và bắt đầu từ con đầu tiên */
        g_comp_parent = find_completion_parent();
        g_comp_cur = g_comp_parent ? g_comp_parent->children : NULL;
    }
    while (g_comp_cur) {
        schema_node_t *n = g_comp_cur;
        g_comp_cur = g_comp_cur->next;
        if (strncasecmp(n->name, text, strlen(text)) == 0)
            return strdup(n->name);
    }
    return NULL;
}

/**
 * show_sub_generator - Hàm sinh gợi ý cho lệnh con của "show".
 *
 * Gợi ý "running-config" hoặc "candidate-config" sau khi gõ "show ".
 *
 * @param text   Tiền tố người dùng đã gõ
 * @param state  0 = lần gọi đầu tiên, != 0 = lần gọi tiếp theo
 * @return       Chuỗi gợi ý (malloc'd) hoặc NULL khi hết
 */
static char *show_sub_generator(const char *text, int state) {
    static const char *subs[] = {"running-config", "candidate-config", NULL};
    static int idx;
    if (!state) idx = 0;
    while (subs[idx]) {
        const char *s = subs[idx++];
        if (strncasecmp(s, text, strlen(text)) == 0)
            return strdup(s);
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
    } else if (strcasecmp(cmd, "set") == 0 ||
               strcasecmp(cmd, "unset") == 0) {
        /* Sau "set" hoặc "unset": gợi ý đường dẫn schema YANG */
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

    if (!xml) {
        fprintf(stderr, "%sget-config failed%s\n", COLOR_RED, COLOR_RESET);
        return;
    }

    /* Lọc XML theo đường dẫn nếu người dùng chỉ định (ví dụ: show running-config system ntp) */
    const char **path = (const char **)(args + 1);
    int path_len = argc - 1;

    /* Chuyển đổi XML thô thành dạng text có thụt lề dễ đọc */
    char *text = fmt_xml_to_text(xml, path, path_len);
    free(xml);
    paged_print(text);
    free(text);
    /* Hiển thị thời gian thực hiện */
    printf("%s(%ldms)%s\n", COLOR_DIM, ms, COLOR_RESET);
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
static void cmd_set(char **args, int argc) {
    if (argc == 0) {
        /* Chế độ paste XML: đọc khối XML từ stdin và nạp vào candidate */
        char *xml = read_xml_paste();
        if (!xml) return;
        if (maapi_load_xml(g_maapi, xml) == 0)
            printf("%sOK%s (staged in candidate)\n", COLOR_GREEN, COLOR_RESET);
        else
            fprintf(stderr, "%sload failed%s\n", COLOR_RED, COLOR_RESET);
        free(xml);
        return;
    }

    /*
     * Hai cú pháp:
     *   set system ntp enabled true          <- phân tách bằng dấu cách (giống Go CLI)
     *   set /system/ntp/enabled true         <- keypath ConfD (cú pháp cũ)
     */
    const char *value = NULL;
    char *keypath = NULL;

    if (args[0][0] == '/') {
        /* Cú pháp keypath ConfD: truyền thẳng đường dẫn, không cần chuyển đổi */
        if (argc < 2) {
            printf("Usage: set <path...> <value>\n"
                   "Example: set system ntp enabled true\n"
                   "         set system ntp server 10.0.0.1 prefer true\n");
            return;
        }
        keypath = strdup(args[0]);
        value   = args[1];
    } else {
        /* Cú pháp dấu cách: chuyển đổi chuỗi token thành keypath ConfD qua cây schema */
        int consumed = 0;
        keypath = args_to_keypath(g_schema, args, argc, &consumed);
        if (!keypath || consumed == 0) {
            fprintf(stderr, "%sPath not found in schema%s\n",
                    COLOR_RED, COLOR_RESET);
            free(keypath);
            return;
        }
        /* Kiểm tra xem sau đường dẫn còn token nào làm giá trị hay không */
        if (consumed >= argc) {
            printf("Usage: set <path...> <value>\n"
                   "Example: set system hostname new-name\n");
            free(keypath);
            return;
        }
        value = args[consumed];  /* Token tiếp theo sau đường dẫn là giá trị cần đặt */
    }

    /* Gọi MAAPI để đặt giá trị vào candidate datastore */
    if (maapi_set_value_str(g_maapi, keypath, value) == 0)
        printf("%sOK%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sset failed%s\n", COLOR_RED, COLOR_RESET);
    free(keypath);
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
    if (maapi_delete_node(g_maapi, keypath) == 0)
        printf("%sOK%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sunset failed%s\n", COLOR_RED, COLOR_RESET);
    free(keypath);
}

/**
 * cmd_commit - Xử lý lệnh "commit": áp dụng thay đổi từ candidate sang running.
 *
 * Đo thời gian thực hiện và hiển thị kết quả (thành công hoặc thất bại).
 */
static void cmd_commit(void) {
    struct timeval t0; gettimeofday(&t0, NULL);
    if (maapi_do_commit(g_maapi) == 0)
        printf("%sCommit successful.%s (%ldms)\n",
               COLOR_GREEN, COLOR_RESET, elapsed_ms(&t0));
    else
        fprintf(stderr, "%sCommit failed.%s\n", COLOR_RED, COLOR_RESET);
}

/**
 * cmd_validate - Xử lý lệnh "validate": kiểm tra tính hợp lệ của candidate.
 *
 * Gọi MAAPI validate để ConfD kiểm tra ràng buộc YANG (must, when, unique, v.v.)
 * mà không áp dụng thay đổi.
 */
static void cmd_validate(void) {
    if (maapi_do_validate(g_maapi) == 0)
        printf("%sValidation OK.%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sValidation failed.%s\n", COLOR_RED, COLOR_RESET);
}

/**
 * cmd_discard - Xử lý lệnh "discard": huỷ bỏ mọi thay đổi trong candidate.
 *
 * Đặt lại candidate datastore về trạng thái giống running datastore.
 */
static void cmd_discard(void) {
    if (maapi_do_discard(g_maapi) == 0)
        printf("%sDiscarded.%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sDiscard failed.%s\n", COLOR_RED, COLOR_RESET);
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
    int db = CONFD_CANDIDATE;  /* Mặc định khoá candidate */
    if (argc > 0 && strcasecmp(args[0], "running") == 0) db = CONFD_RUNNING;
    if (maapi_do_lock(g_maapi, db) == 0)
        printf("%sLocked.%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sLock failed.%s\n", COLOR_RED, COLOR_RESET);
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
    int db = CONFD_CANDIDATE;  /* Mặc định mở khoá candidate */
    if (argc > 0 && strcasecmp(args[0], "running") == 0) db = CONFD_RUNNING;
    if (maapi_do_unlock(g_maapi, db) == 0)
        printf("%sUnlocked.%s\n", COLOR_GREEN, COLOR_RESET);
    else
        fprintf(stderr, "%sUnlock failed.%s\n", COLOR_RED, COLOR_RESET);
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

/**
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
 * Luồng thực hiện chính:
 *   1. Cài đặt xử lý tín hiệu SIGINT (Ctrl+C)
 *   2. Cấu hình readline (tab-completion, lịch sử lệnh)
 *   3. Đọc cấu hình từ biến môi trường (địa chỉ, cổng, tên người dùng, tên NE)
 *   4. Kết nối tới ConfD qua MAAPI
 *   5. Tải cây schema YANG từ ConfD (phục vụ tab-completion và chuyển đổi đường dẫn)
 *   6. Vòng lặp chính: đọc lệnh từ readline -> dispatch -> xử lý
 *   7. Dọn dẹp tài nguyên khi thoát
 *
 * @return  0 nếu thành công, 1 nếu không kết nối được tới ConfD
 */
int main(void) {
    /* Cài đặt handler cho SIGINT để Ctrl+C không thoát chương trình */
    signal(SIGINT, sigint_handler);
    rl_catch_signals = 0;  /* Tắt xử lý tín hiệu mặc định của readline, dùng handler riêng */
    rl_attempted_completion_function = maapi_completer;  /* Đăng ký hàm tab-completion */
    rl_variable_bind("expand-tilde", "off");  /* Tắt mở rộng dấu ~ trong readline */

    /* Đọc cấu hình kết nối từ biến môi trường */
    const char *host = env_or("CONFD_IPC_ADDR", "127.0.0.1");
    int         port = env_int_or("CONFD_IPC_PORT", CONFD_PORT);
    const char *user = env_or("MAAPI_USER", "admin");
    strncpy(g_ne_name, env_or("NE_NAME", "confd"), sizeof(g_ne_name) - 1);

    /* Hiển thị banner chào mừng */
    printf("%s", COLOR_GREEN);
    printf("=====================================================\n");
    printf("   CLI - NETCONF Console (C / MAAPI Direct Mode)\n");
    printf("=====================================================%s\n\n", COLOR_RESET);
    printf("Connecting to ConfD MAAPI %s%s:%d%s ...\n",
           COLOR_BOLD, host, port, COLOR_RESET);

    /* Thiết lập kết nối MAAPI tới ConfD */
    g_maapi = maapi_dial(host, port, user);
    if (!g_maapi) {
        fprintf(stderr, "%sMAAPI connect failed. Is ConfD running?%s\n",
                COLOR_RED, COLOR_RESET);
        return 1;
    }

    printf("%sConnected.%s\n", COLOR_GREEN, COLOR_RESET);

    /* Tải cây schema YANG từ ConfD qua MAAPI để hỗ trợ tab-completion
     * và chuyển đổi đường dẫn dấu cách sang keypath ConfD */
    printf("Loading schema from MAAPI...\n");
    g_schema = schema_new_node("__root__");
    if (g_schema) {
        maapi_load_schema_into(g_maapi, &g_schema);
        printf("Schema loaded.\n");
    }

    /* Khởi tạo prompt và lịch sử lệnh readline */
    update_prompt();
    using_history();
    stifle_history(HISTORY_SIZE);  /* Giới hạn số lệnh lưu trong lịch sử */

    printf("Type %shelp%s for commands. Ctrl+D or %sexit%s to quit.\n\n",
           COLOR_CYAN, COLOR_RESET, COLOR_CYAN, COLOR_RESET);

    /* ── Vòng lặp chính: đọc và xử lý lệnh ── */
    char *line;
    while ((line = readline(g_prompt)) != NULL) {
        char *trimmed = str_trim(line);  /* Loại bỏ khoảng trắng đầu/cuối */
        if (*trimmed) {
            add_history(trimmed);  /* Lưu lệnh vào lịch sử (cho phím mũi tên lên/xuống) */
            /* Kiểm tra lệnh thoát */
            if (strcasecmp(trimmed, "exit") == 0 ||
                strcasecmp(trimmed, "quit") == 0) {
                free(line);
                break;
            }
            dispatch(trimmed);  /* Phân phối lệnh tới hàm xử lý tương ứng */
        }
        free(line);
    }

    /* Dọn dẹp tài nguyên trước khi thoát */
    printf("Goodbye.\n");
    schema_free(g_schema);          /* Giải phóng cây schema */
    cli_session_close(g_maapi);     /* Đóng phiên MAAPI và giải phóng kết nối */
    return 0;
}
