# CLI - NETCONF Console

CLI cho phép user quản lý cấu hình các Network Element (NE) trong hệ thống 5GC thông qua giao thức NETCONF (RFC 6241).

Có hai implementation:
- **`go/`** — Go: SSH server (production) + direct mode; xác thực qua mgt-service
- **`c/`** — C: direct mode nhanh, tab completion dựa trên YANG schema; hỗ trợ MAAPI (ConfD)

## Kiến trúc

```
User --SSH--> [CLI Pod :2222]          (Go implementation)
                |
                |-- Auth -----> [mgt-service :3000]  (REST API, JWT)
                |-- List NE --> [mgt-service :3000]
                |-- History --> [mgt-service :3000]
                |-- Backup ---> [mgt-service :3000]  (lưu/load config snapshots)
                |
                |-- NETCONF/SSH --> [NE Pod :830]  (ConfD, YANG model)

User --stdin-> [cli-netconf-c]         (C implementation)
                |
                |-- Auth -----> [mgt-service :3000]  (REST API, JWT)
                |-- NETCONF/TCP --> [NE :2023]   (plain TCP, debug)
                |-- NETCONF/SSH --> [NE :830]    (WITH_SSH=1)
                |-- MAAPI -------> [ConfD :4565] (WITH_MAAPI=1, schema)
```

## Cấu trúc project

```
cli-netconf/
├── go/                               # Go implementation
│   ├── cmd/
│   │   ├── netconf/main.go           # SSH server (production)
│   │   ├── direct/main.go            # Direct mode (TCP hoặc SSH, env NETCONF_MODE)
│   │   ├── direct-ssh/main.go        # Direct SSH cố định
│   │   └── direct-tcp/main.go        # Direct TCP cố định
│   ├── pkg/
│   │   ├── config/config.go          # Cấu hình từ env vars
│   │   ├── api/client.go             # HTTP client mgt-service API
│   │   ├── netconf/client.go         # NETCONF client (SSH + TCP)
│   │   └── server/
│   │       ├── server.go             # SSH server, graceful shutdown
│   │       ├── session.go            # Shell, session lifecycle, backup
│   │       ├── direct.go             # RunDirect — stdio session
│   │       ├── cmd_general.go        # show, connect, disconnect, help, loadSchema
│   │       ├── cmd_netconf.go        # set, unset, commit, dump, lock, rpc, restore
│   │       ├── completer.go          # Tab completion, YANG parser, schema tree
│   │       └── formatter.go          # XML↔text, dump formats
│   ├── test/
│   │   ├── yang/ne-system.yang       # YANG model mẫu
│   │   ├── mock-netconf/main.go      # Mock NETCONF server (SSH :8830 + TCP :2023)
│   │   ├── mock-mgt/main.go          # Mock mgt-service API
│   │   ├── e2e_test.go               # 28 e2e tests
│   │   └── run.sh                    # Script chạy tất cả services
│   ├── deploy/k8s.yaml               # K8s Deployment + Service
│   ├── Dockerfile                    # Multi-stage build
│   ├── Makefile
│   ├── mgt-service.yaml              # OpenAPI spec
│   ├── go.mod
│   └── go.sum
│
├── c/                                # C implementation
│   ├── include/cli.h                 # Tất cả types & declarations
│   ├── src/
│   │   ├── main.c                    # Entry point, auth, session loop
│   │   ├── netconf.c                 # NETCONF client (TCP + SSH tùy chọn)
│   │   ├── auth.c                    # mgt-service REST API (libcurl)
│   │   ├── schema.c                  # Schema tree từ YANG/XML; ns-aware
│   │   ├── maapi.c                   # ConfD MAAPI provider (WITH_MAAPI=1)
│   │   ├── completer.c               # GNU readline tab completion
│   │   ├── formatter.c               # XML↔text, pager
│   │   └── commands.c                # Tất cả command handlers
│   └── Makefile
│
├── Makefile                          # Root Makefile (build-go / build-c / test)
└── README.md
```

## Tính năng

### Xác thực & Chọn NE
- Đăng nhập bằng username/password
- Xác thực qua mgt-service API (`POST /aa/authenticate`)
- Hiển thị danh sách NE, chọn bằng số hoặc tên
- Hỗ trợ multi-session (Go SSH server)

### Điều hướng session

| Tình huống | Ctrl+C | `exit` |
|---|---|---|
| Đang gõ lệnh (kết nối NE) | Huỷ dòng đang gõ | Ngắt kết nối → quay về chọn NE |
| Màn hình chọn NE | Huỷ dòng đang gõ | Thoát hoàn toàn |

Lệnh `disconnect` ngắt kết nối NE và quay về màn hình chọn NE.

### Xem cấu hình
- `show running-config` — xem toàn bộ config đang chạy
- `show running-config system ntp` — filter theo path
- `show candidate-config` — xem bản nháp chưa commit
- Output có indent, list entry hiển thị key value (ví dụ: `interface eth0`)

### Thiết lập cấu hình
| Lệnh | Mô tả |
|---|---|
| `set <path...> <value>` | Set giá trị theo path |
| `set` | Paste config (XML hoặc text format) |
| `unset <path...>` | Xoá config node |
| `commit` | Áp dụng candidate vào running |
| `validate` | Kiểm tra candidate trước khi commit |
| `discard` | Huỷ bỏ thay đổi candidate |
| `lock [datastore]` | Khoá datastore |
| `unlock [datastore]` | Mở khoá datastore |

### Tab Completion (schema-aware)
- Tab complete lệnh, sub-command, config path
- Schema load từ YANG (get-schema RFC 6022) + XML fallback + MAAPI (C)
- `show running-config system <TAB>` → `contact  dns  hostname  location  logging  ntp`
- Correct depth: `qosConf` chỉ xuất hiện dưới `vsmf`, không phải `nsmfPduSession`

### Export config
| Lệnh | Mô tả |
|---|---|
| `dump text [filename]` | Export config dạng text indent |
| `dump xml [filename]` | Export config dạng XML |

### Backup & Restore config
Mỗi lần `commit` thành công, CLI tự động lưu snapshot lên mgt-service.

| Lệnh | Mô tả |
|---|---|
| `show backups` | Liệt kê snapshot với ID, thời gian, nguồn |
| `restore <id>` | Roll back về snapshot đó |

### Pager
Output dài hơn 20 dòng sẽ được phân trang:

| Phím | Hành động |
|---|---|
| **Enter** | Trang tiếp theo |
| **a** / **G** | Hiện toàn bộ phần còn lại |
| **q** | Thoát pager |

## Cài đặt & Build

### Yêu cầu

**Go implementation:**
- Go 1.21+

**C implementation:**
- macOS: `brew install readline` (libssh2 tuỳ chọn)
- Linux: `apt install libcurl4-openssl-dev libxml2-dev libreadline-dev`
- libssh2 (tuỳ chọn): `brew install libssh2` / `apt install libssh2-1-dev`
- libconfd (tuỳ chọn, cho MAAPI): ConfD SDK

### Build nhanh

```bash
# Build cả hai
make all

# Chỉ Go
make build-go   # → go/bin/cli-netconf, go/bin/cli-direct, ...

# Chỉ C
make build-c    # → c/cli-netconf-c
```

### Build C với tuỳ chọn

```bash
# Chỉ TCP (mặc định)
make -C c

# Thêm SSH support
make -C c WITH_SSH=1

# Thêm MAAPI (cần ConfD SDK)
make -C c WITH_MAAPI=1 CONFD_DIR=/path/to/confd
```

### Build Go chi tiết

```bash
cd go

# SSH server (production)
go build -o bin/cli-netconf ./cmd/netconf

# Direct mode (TCP hoặc SSH qua env NETCONF_MODE)
go build -o bin/cli-direct ./cmd/direct
```

## Cấu hình

### Go — SSH server (`cmd/netconf`)

| Biến | Mặc định | Mô tả |
|---|---|---|
| `SSH_ADDR` | `:2222` | Địa chỉ SSH server |
| `MGT_SERVICE_URL` | `http://mgt-service:3000` | URL mgt-service |
| `NETCONF_USERNAME` | `admin` | Account kết nối NETCONF |
| `NETCONF_PASSWORD` | `admin` | Mật khẩu NETCONF |
| `NETCONF_TIMEOUT` | `30s` | Timeout kết nối |

### C — Direct mode

| Biến | Mặc định | Mô tả |
|---|---|---|
| `MGT_URL` | `http://127.0.0.1:3000` | URL mgt-service |
| `NETCONF_HOST` | _(từ NE list)_ | Override host |
| `NETCONF_PORT` | _(từ NE list)_ | Override port |
| `NETCONF_MODE` | `tcp` | `tcp` hoặc `ssh` |
| `NETCONF_USER` | `admin` | SSH username |
| `NETCONF_PASS` | `admin` | SSH password |
| `CONFD_IPC_ADDR` | `127.0.0.1` | MAAPI host (WITH_MAAPI) |
| `CONFD_IPC_PORT` | `4565` | MAAPI port (WITH_MAAPI) |

## Chạy với mock servers

```bash
# Khởi động mock servers
cd go
go run ./test/mock-mgt &       # mgt-service :3000
go run ./test/mock-netconf &   # NETCONF SSH :8830, TCP :2023

# Chạy Go (SSH server)
make run-go

# Kết nối SSH
ssh admin@127.0.0.1 -p 2222   # password: admin

# Chạy C (direct mode, TCP)
make run-c
```

Hoặc dùng script tự động:
```bash
cd go && bash test/run.sh
```

## Testing

```bash
# Go e2e tests (28 tests)
make test-go
# hoặc: cd go && go test -v -timeout 300s ./test/

# C — test thủ công với mock
make run-c
```

### Test cases Go (28 tests)

| # | Test | Mô tả |
|---|---|---|
| 1 | WelcomeBannerAndNEList | Banner + auto NE list |
| 2 | ConnectByName | Kết nối NE bằng tên |
| 3 | ShowNE | Danh sách NE |
| 4 | ShowRunningConfig | Xem toàn bộ config |
| 5 | ShowRunningConfigPathHeader | Full path prefix |
| 6 | SetInline | set system hostname value |
| 7 | SetXML | Paste XML config |
| 8 | SetTextConfigPaste | Paste text format → XML |
| 9 | SetAndCommit | Set + commit flow |
| 10 | Unset | Xoá config node |
| 11 | Validate | Validate candidate |
| 12 | Discard | Discard changes |
| 13 | LockUnlock | Lock/unlock datastore |
| 14 | DumpText | Export text file |
| 15 | DumpXML | Export XML file |
| 16 | DumpToTerminal | Dump ra terminal |
| 17 | TabCompleteCommand | Tab lệnh |
| 18 | TabCompleteShowSubcommand | Tab sub-command |
| 19 | TabCompleteConfigPath | Tab config path |
| 20 | TabCompleteSet | Tab cho set |
| 21 | TabCompleteUnset | Tab cho unset |
| 22 | TabCompleteMultipleOptions | Nhiều options |
| 23 | Help | Help text |
| 24 | UnknownCommand | Lệnh sai |
| 25 | Stress500Configs | 500 interfaces (127KB) |
| 26 | Stress1000Configs | 1000 interfaces (253KB) |
| 27 | MultiSession | 5 SSH sessions đồng thời |

## Deploy K8s (Go)

```bash
cd go

# Secrets
ssh-keygen -t ed25519 -f host_key -N ""
kubectl create secret generic cli-netconf-host-key --from-file=host_key
kubectl create secret generic cli-netconf-secret \
  --from-literal=netconf-username=admin \
  --from-literal=netconf-password=<password>

# Build & deploy
docker build -t cli-netconf .
kubectl apply -f deploy/k8s.yaml
```

## Cách sử dụng

```
============================================
        CLI - NETCONF Console
============================================

  #  NE          Site  IP         Port  Description
  1  ne-amf-01   HCM   10.0.1.10  830   AMF Node
  2  ne-smf-01   HNI   10.0.1.20  830   SMF Node

Select NE [1-2 or name] (exit to quit): 1
Connected. NETCONF session ID: 42

admin[ne-amf-01]> show running-config system ntp
  enabled               true
  server 10.0.0.1
    prefer              true

admin[ne-amf-01]> set system hostname new-name
OK

admin[ne-amf-01]> commit
Commit successful.

admin[ne-amf-01]> show backups
  ID  Timestamp              Size
   1  2026-04-14 09:00:00    local

admin[ne-amf-01]> exit
```
