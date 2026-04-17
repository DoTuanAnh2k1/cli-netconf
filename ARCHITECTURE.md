# Architecture — CLI NETCONF

Tài liệu giải thích code theo flow thực thi, tổ chức phân tầng từ trên xuống dưới.

---

## Tổng quan phân tầng

```
┌─────────────────────────────────────────────────────────┐
│  Layer 1: Entry & CLI Loop              (main.c)        │
│  Khởi tạo, readline loop, dispatch lệnh                │
├─────────────────────────────────────────────────────────┤
│  Layer 2: Authentication & NE Selection (main.c)        │
│  HTTP client, mgt-svc auth, NE list, interactive select │
├─────────────────────────────────────────────────────────┤
│  Layer 3: Command Handlers              (main.c)        │
│  cmd_show, cmd_set, cmd_commit, cmd_dump, ...           │
├─────────────────────────────────────────────────────────┤
│  Layer 4: MAAPI Operations              (maapi-ops.c)   │
│  Session, get/set config, commit, lock, rollback        │
├─────────────────────────────────────────────────────────┤
│  Layer 5: Schema & Formatting                           │
│  schema.c (tree build/lookup), formatter.c (XML↔text)   │
│  maapi.c (schema load từ ConfD cs_node)                 │
├─────────────────────────────────────────────────────────┤
│  Layer 6: ConfD Compatibility           (confd_compat.h)│
│  ABI shim thay thế ConfD SDK headers                    │
└─────────────────────────────────────────────────────────┘
         │
         ▼
   [libconfd.so]  ──IPC──→  [ConfD NE]
```

---

## Flow 1: Khởi động (main)

```
main()                                          ← main.c
  │
  ├── signal(SIGINT, sigint_handler)            Ctrl+C không thoát, chỉ clear dòng
  ├── signal(SIGPIPE, SIG_IGN)                  Tránh crash khi mgt-svc đóng socket
  ├── rl_attempted_completion_function = ...     Đăng ký tab completion
  │
  ├── if CONFD_IPC_ADDR set?
  │   ├── YES → maapi_dial() → load schema     Direct mode: kết nối ngay
  │   └── NO  → print "Use login"              Login mode: chờ user login
  │
  ├── update_prompt()                           "maapi[confd]> "
  ├── using_history()                           Readline history
  │
  └── while readline(prompt):                   ─── Vòng lặp chính ───
        ├── str_trim(line)
        ├── add_history(trimmed)
        ├── if "exit"/"quit" → break
        └── dispatch(trimmed)                   Phân phối lệnh
```

**Điểm quan trọng**: Khi không set `CONFD_IPC_ADDR`, CLI khởi động mà không cần
ConfD. User phải `login` → chọn NE → lúc đó mới kết nối MAAPI.

---

## Flow 2: Login → Chọn NE → Kết nối

```
cmd_login(args, argc)                           ← main.c
  │
  ├── read_password()                           Echo tắt, giống ssh prompt
  │     └── tcsetattr(~ECHO) → getline → restore
  │
  ├── json_escape(user, password)               Escape cho JSON body
  │
  ├── http_post_json(                           ── HTTP POST ──
  │     "/aa/authenticate",
  │     {"username":"...","password":"..."})
  │     │
  │     ├── parse_url() → host, port, path
  │     ├── getaddrinfo() → DNS resolve
  │     ├── socket() + connect_with_timeout()   3s timeout, non-blocking
  │     ├── send() với MSG_NOSIGNAL             Không SIGPIPE
  │     ├── recv() loop (4KB buffer)
  │     └── parse HTTP status + body
  │
  ├── json_extract_string(resp, "response_data")  Lấy JWT token
  ├── json_extract_string(resp, "status")         Check "success"
  ├── g_mgt_token = token                         Lưu global
  │
  │   ── Sau login thành công ──
  │
  ├── fetch_ne_list(token)                      ── GET /aa/list/ne ──
  │     │
  │     ├── http_get_json() với Authorization header
  │     ├── Parse JSON: tìm "neDataList" → duyệt [{...}, {...}]
  │     └── Mỗi object → ne_item_t:
  │           site, ne, ip, namespace,
  │           conf_master_ip, conf_port_master_tcp    ← dùng để kết nối
  │
  ├── display_ne_list(nes, count)               In bảng NE
  │     │
  │     │  #   Site   NE    Namespace   ConfD IP       Port
  │     │  1   HCM    eir   hteir01     172.19.0.2     23645
  │
  ├── select_ne_interactive(nes, count)         ── Chọn NE ──
  │     │
  │     └── loop:
  │           readline("Select NE (# or namespace): ")
  │           ├── Thử parse số → check range 1..count
  │           ├── So sánh namespace (case-insensitive)
  │           ├── Nếu sai → "Invalid selection" → hỏi lại
  │           └── Ctrl+D → return -1 (huỷ)
  │
  │   ── Kết nối tới NE đã chọn ──
  │
  ├── conn_ip   = conf_master_ip  (fallback: ip)
  ├── conn_port = conf_port_master_tcp (fallback: port)
  │
  ├── cli_session_close(g_maapi)                Đóng session cũ (nếu có)
  ├── schema_free(g_schema)                     Giải phóng schema cũ
  │
  ├── maapi_dial(conn_ip, conn_port, user)      ── Kết nối MAAPI mới ──
  │     │                                       (xem Flow 4 bên dưới)
  │
  ├── g_ne_name = chosen->ne                    Cập nhật tên NE
  ├── update_prompt()                           "maapi[eir]> "
  │
  └── maapi_load_schema_into(g_maapi, &g_schema)  Load schema từ ConfD mới
```

---

## Flow 3: Dispatch lệnh

```
dispatch(line)                                  ← main.c
  │
  ├── str_split(line, &argc)                    Tách token bằng whitespace
  │
  └── switch argv[0]:                           Case-insensitive
        "show"     → cmd_show(args, argc)
        "set"      → cmd_set(args, argc)
        "unset"    → cmd_unset(args, argc)
        "commit"   → cmd_commit()
        "validate" → cmd_validate()
        "discard"  → cmd_discard()
        "lock"     → cmd_lock(args, argc)
        "unlock"   → cmd_unlock(args, argc)
        "dump"     → cmd_dump(args, argc)
        "rollback" → cmd_rollback(args, argc)
        "login"    → cmd_login(args, argc)
        "logout"   → cmd_logout()
        "save"     → cmd_save(args, argc)
        "nodes"    → cmd_nodes()
        "help"     → cmd_help()
        default    → "Unknown command"
```

---

## Flow 4: show running-config (đọc config)

```
cmd_show(args, argc)                            ← main.c
  │
  ├── Parse "running-config" / "candidate-config"
  │     → db = CONFD_RUNNING hoặc CONFD_CANDIDATE
  │
  ├── maapi_get_config_xml(g_maapi, db)         ← maapi-ops.c
  │     │
  │     ├── ensure_write_trans(m)               Mở transaction nếu chưa có
  │     │     └── maapi_start_trans2()          Gọi libconfd
  │     │
  │     ├── maapi_save_config()                 Stream config XML qua socket
  │     │     ├── confd_stream_connect()        Mở stream socket tới ConfD
  │     │     ├── maapi_save_config()           Bắt đầu stream
  │     │     │   flags: MAAPI_CONFIG_XML | MAAPI_CONFIG_SHOW_DEFAULTS
  │     │     ├── read() loop                   Đọc XML từ stream socket
  │     │     └── close(stream_sock)
  │     │
  │     └── return xml_string                   Toàn bộ config dạng XML
  │
  ├── Có path filter? (vd: "show running-config eir nsmfPduSession")
  │     └── args_to_keypath(schema, args, argc) ← maapi-ops.c
  │           Chuyển "eir nsmfPduSession" → "/eir/nsmfPduSession"
  │
  ├── fmt_xml_to_text(xml, path, path_len)      ← formatter.c
  │     │
  │     ├── xmlParseMemory(xml)                 Parse XML bằng libxml2
  │     ├── Navigate tới path node
  │     ├── render_node() recursive             Render XML tree → text
  │     │     ├── Container/list → "name {"
  │     │     ├── Leaf → "name  value"
  │     │     └── Indent theo depth
  │     └── return text_string
  │
  └── paged_print(text)                         ← main.c
        │
        ├── get_terminal_rows()                 ioctl TIOCGWINSZ
        ├── Print từng dòng
        ├── Mỗi (rows-1) dòng → "<MORE>" prompt
        │     q/Q → dừng, Enter/Space → tiếp
        └── In elapsed time "(45ms)"
```

---

## Flow 5: set + commit (ghi config)

```
cmd_set(args, argc)                             ← main.c
  │
  ├── argc == 0?
  │     └── YES → read_xml_paste()              Đọc XML multi-line từ stdin
  │                └── maapi_load_xml(xml)      ← maapi-ops.c
  │                      ├── write_tmp_str(xml) Ghi XML ra file tạm
  │                      ├── maapi_load_config()  Load vào candidate
  │                      │   flag: MAAPI_CONFIG_XML | MAAPI_CONFIG_MERGE
  │                      └── unlink(tmpfile)
  │
  └── argc >= 2?
        ├── args_to_keypath(args[0..n-2])       Chuyển path → keypath
        ├── value = args[n-1]                   Giá trị cuối cùng
        └── maapi_set_value_str(keypath, value) ← maapi-ops.c
              ├── ensure_write_trans()
              └── maapi_set_elem2()             Gọi libconfd: set giá trị


cmd_commit()                                    ← main.c
  │
  └── maapi_do_commit(g_maapi)                  ← maapi-ops.c
        ├── maapi_apply_trans()                 Apply candidate → running
        ├── maapi_finish_trans()                Đóng transaction
        └── m->has_write = false                Reset state
```

---

## Flow 6: Tab completion

```
maapi_completer(text, start, end)               ← main.c
  │  (Được gọi bởi readline khi user nhấn TAB)
  │
  ├── Parse dòng lệnh hiện tại
  │
  ├── start == 0?                               Đang gõ command name
  │     └── rl_completion_matches(cmd_generator)
  │           → "show", "set", "commit", "login", ...
  │
  ├── command == "show" && word 1?
  │     └── generator: "running-config", "candidate-config"
  │
  ├── command == "show" && word >= 2?
  │     └── find_completion_parent(schema, words)
  │           │
  │           ├── schema_lookup(root, path, depth)  ← schema.c
  │           │     Walk schema tree theo path đang gõ
  │           │
  │           └── path_generator()
  │                 Duyệt children của node → trả từng tên match prefix
  │
  └── command == "lock"/"unlock"?
        └── generator: "running", "candidate"
```

---

## Flow 7: Schema loading

```
maapi_load_schema_into(m, &out_schema)          ← maapi.c
  │
  ├── confd_load_schemas()                      Gọi libconfd: load tất cả YANG
  │     → confd_find_cs_root() → cs_node root
  │
  ├── Với mỗi namespace:
  │     confd_nsinfo(hash) → namespace info
  │     confd_find_cs_root(ns)
  │
  └── walk_cs_node(cs_root, schema_root)        Recursive walk
        │
        ├── Bỏ qua: operations, notifications, config false
        │
        ├── schema_new_node(name)               ← schema.c
        │     Tạo node mới trong schema tree
        │
        ├── node->is_list = CS_NODE_IS_LIST
        ├── node->is_leaf = !(container || list)
        │
        └── Recurse children


Schema tree structure (schema.c):

  schema_node_t (linked list)
  ┌──────────────┐
  │ name = "eir" │
  │ ns = "..."   │
  │ is_list = 0  │
  │ is_leaf = 0  │
  │ children ────┼──→ ┌────────────────────┐
  │ next ────────┼──→ │ name = "nacm"      │
  └──────────────┘    │ children → ...      │
                      │ next → ...          │
    ┌─────────────────┘
    ▼
  ┌──────────────────────┐     ┌───────────────────┐
  │ name="nsmfPduSession"│──→  │ name="networkCfg" │──→ NULL
  │ children → ...       │     │ children → ...    │
  └──────────────────────┘     └───────────────────┘
```

---

## Flow 8: XML ↔ Text formatting

```
fmt_xml_to_text(xml, path, path_len)            ← formatter.c
  │
  │  Input (XML từ ConfD):
  │  <eir xmlns="...">
  │    <nsmfPduSession>
  │      <is-enabled>true</is-enabled>
  │    </nsmfPduSession>
  │  </eir>
  │
  ├── xmlParseMemory(xml)                       libxml2 parse
  ├── Navigate tới path node (nếu có filter)
  │
  └── render_node(node, depth)                  Recursive render
        │
        ├── Element có children?
        │     "nsmfPduSession {"
        │     │  render_node(child1, depth+1)
        │     │  render_node(child2, depth+1)
        │     "}"
        │
        └── Leaf (text content)?
              "  is-enabled               true"
              (pad tên đến 25 ký tự, align giá trị)

  Output (text):
    nsmfPduSession {
      is-enabled               true
      status                   active
    }
```

---

## HTTP Client (main.c)

CLI có HTTP client tự viết bằng socket (không dùng libcurl):

```
http_post_json(url, body, headers)
http_get_json(url, headers)
  │
  ├── parse_url("http://host:port/path")
  ├── getaddrinfo(host, port)                   DNS resolve
  ├── socket(AF_UNSPEC, SOCK_STREAM)
  ├── connect_with_timeout(3s)                  Non-blocking + poll()
  ├── setsockopt(SO_SNDTIMEO, SO_RCVTIMEO, 5s) I/O timeout
  │
  ├── send(request_header + body, MSG_NOSIGNAL)
  │
  ├── recv() loop → buffer
  ├── Parse "HTTP/1.1 200 OK"
  ├── Find "\r\n\r\n" → body
  └── return (status, body)
```

**Bảo đảm an toàn:**
- `MSG_NOSIGNAL`: không bao giờ raise SIGPIPE
- Connect timeout 3s: không hang trên host chết
- I/O timeout 5s: không treo khi server câm
- Mọi lỗi malloc/parse được xử lý, không leak

---

## ConfD Compatibility Layer (confd_compat.h)

Thay thế toàn bộ ConfD SDK headers bằng 1 file duy nhất:

```
confd_compat.h
  │
  ├── Struct layout khớp ABI libconfd.so:
  │     confd_value, confd_cs_node, confd_nsinfo, ...
  │
  ├── Constants:
  │     CONFD_PORT, CONFD_RUNNING, CONFD_CANDIDATE, ...
  │     MAAPI_CONFIG_XML, CS_NODE_IS_LIST, ...
  │
  ├── Function declarations (extern → link với libconfd.so):
  │     confd_init(), confd_load_schemas()
  │     maapi_connect(), maapi_start_user_session()
  │     maapi_start_trans2(), maapi_apply_trans()
  │     maapi_save_config(), maapi_set_elem2()
  │     maapi_load_config(), maapi_delete()
  │     ...
  │
  └── Versioned symbols (__asm__(".symver ...")):
        Khớp đúng symbol version trong .so
```

**Tại sao cần?** ConfD SDK headers không public. File này reverse-engineer
đúng struct layout và symbol versions để link trực tiếp với `libconfd.so`
mà không cần cài SDK.

---

## Logging (log.h)

```
LOG_INFO("connected to %s:%d", host, port)
  │
  ├── Macro expand → if (g_log_level <= LOG_LVL_INFO) log_write(...)
  │
  ├── Khi LOG_LEVEL=off → g_log_level = LOG_LVL_OFF
  │     → if() luôn false → compiler optimize bỏ hẳn → zero overhead
  │
  └── log_write(level, file, line, fmt, ...)
        ├── timestamp: 2026-04-18 02:30:48
        ├── level: [INFO ]
        ├── source: main.c:1602
        ├── message: connected to 172.19.0.2:23645
        └── fprintf(g_log_fp, ...)
```

SSH server mode: mỗi user ghi log riêng → `/var/log/cli-netconf/<username>.log`

---

## Tổng kết file → trách nhiệm

| File | Tầng | Trách nhiệm |
|---|---|---|
| `main.c` | 1-3 | Entry, CLI loop, dispatch, tab completion, HTTP client, auth, NE selection |
| `maapi-ops.c` | 4 | MAAPI session (dial/close), config read/write, commit/lock/rollback |
| `maapi.c` | 5 | Schema load từ ConfD (walk cs_node tree) |
| `schema.c` | 5 | Schema tree data structure, lookup, YANG/XML parser |
| `formatter.c` | 5 | XML → text renderer, text → XML builder, RPC reply parser |
| `cli.h` | 5-6 | Shared types (`schema_node_t`), constants, color macros |
| `log.h` | 6 | Logging macros + file I/O |
| `maapi-direct.h` | 4 | `maapi_session_t` type, function declarations |
| `confd_compat.h` | 6 | ABI shim cho libconfd.so (thay ConfD SDK headers) |

---

## Luồng dữ liệu tổng thể

```
                          mgt-svc (REST API)
                              │
                    ┌─────────┴──────────┐
                    │  POST /authenticate │
                    │  GET  /list/ne      │
                    └─────────┬──────────┘
                              │ HTTP (JSON)
                              │
User ──stdin──→ [readline] → [dispatch] → [cmd_*]
                    │              │           │
                    │         tab complete     │
                    │              │           │
                    │     [schema tree]    [maapi-ops]
                    │     (schema.c)           │
                    │              │           │ MAAPI IPC
                    │              │           │
                    │     [cs_node walk]   [libconfd.so]
                    │     (maapi.c)            │
                    │                          │
                    │                    [ConfD NE]
                    │                     (config DB)
                    │
                    ├── [formatter.c]
                    │     XML ↔ text
                    │
                    └── stdout → User
```
