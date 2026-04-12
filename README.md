# VHT CLI - NETCONF Console

CLI cho phép user SSH vào để quản lý cấu hình các Network Element (NE) trong hệ thống 5GC thông qua giao thức NETCONF (RFC 6241).

## Kiến trúc

```
User --SSH--> [CLI Pod :2222]
                |
                |-- Auth -----> [mgt-service :3000]  (REST API, JWT)
                |-- List NE --> [mgt-service :3000]
                |-- History --> [mgt-service :3000]
                |
                |-- NETCONF/SSH --> [NE Pod :830]  (ConfD, YANG model)
```

- **CLI Pod**: SSH server, xác thực user qua mgt-service, cung cấp interactive shell
- **mgt-service**: REST API quản lý user, phân quyền, danh sách NE
- **NE Pod**: Network Element chạy ConfD với NETCONF subsystem

## Tính năng

### Xác thực & Chọn NE
- SSH vào CLI bằng username/password
- Xác thực qua mgt-service API (`POST /aa/authenticate`)
- Khi login tự động hiển thị danh sách NE và yêu cầu chọn (bằng số hoặc tên)
- Nhập sai tên NE sẽ báo lỗi và yêu cầu nhập lại
- Hỗ trợ multi-session: nhiều user SSH đồng thời, mỗi session độc lập

### Xem cấu hình
- `show running-config` — xem toàn bộ config đang chạy
- `show running-config system ntp` — filter theo path, có full path header
- `show candidate-config` — xem bản nháp chưa commit
- Output dạng text có indent, list entry hiển thị key value (ví dụ: `interface eth0`)
- Khi filter, dòng đầu hiển thị full path: `system hostname ne-amf-01`

### Thiết lập cấu hình
| Lệnh | Mô tả |
|---|---|
| `set <path...> <value>` | Set giá trị theo path (ví dụ: `set system hostname new-name`) |
| `set` | Paste config (hỗ trợ cả XML và text format) |
| `unset <path...>` | Xoá config node (ví dụ: `unset system contact`) |
| `commit` | Áp dụng candidate vào running |
| `validate` | Kiểm tra candidate trước khi commit |
| `discard` | Huỷ bỏ thay đổi candidate |
| `lock [datastore]` | Khoá datastore |
| `unlock [datastore]` | Mở khoá datastore |

### Paste config dạng text
Có thể copy output của `show running-config` rồi paste ngược lại vào `set`:

```
admin[ne-amf-01]> set
Enter config (XML or text format, end with '.' on a new line):
system
  hostname                 new-hostname
  location                 New Location
  ntp
    enabled                true
.
OK
```

CLI tự động nhận diện text format (không có `<` `>`) và convert sang XML.

### Export config
| Lệnh | Mô tả |
|---|---|
| `dump text [filename]` | Export config dạng text indent |
| `dump xml [filename]` | Export config dạng XML |

Không có filename sẽ output ra terminal.

### Tab Completion
- Tab complete lệnh: `sh<TAB>` → `show`
- Tab complete sub-command: `show r<TAB>` → `show running-config`
- Tab complete config path: `show running-config system n<TAB>` → `ntp`
- Tab complete cho `set`, `unset`: `set system host<TAB>` → `hostname`
- Tab complete cho `dump`: `dump t<TAB>` → `text`
- Schema tự động load từ YANG (get-schema RFC 6022) + running config
- Thấy được tất cả leaf/container kể cả chưa set giá trị

### Graceful Shutdown
- Server nhận SIGINT/SIGTERM → đóng tất cả active SSH sessions → thoát sạch
- Timeout 10s cho shutdown, không bị treo nếu có user đang kết nối

### Lịch sử & Logging
- Tự động lưu command history qua mgt-service (`POST /aa/history/save`)
- Hiển thị thời gian thực thi mỗi lệnh

## Cấu trúc project

```
cli-netconf/
├── main.go                           # Entry point, graceful shutdown
├── internal/
│   ├── config/config.go              # Cấu hình từ env vars
│   ├── api/client.go                 # HTTP client cho mgt-service API
│   ├── netconf/client.go             # NETCONF over SSH client (RFC 6241)
│   └── server/
│       ├── server.go                 # SSH server, graceful shutdown
│       ├── session.go                # Interactive shell, session lifecycle
│       ├── cmd_general.go            # show, connect, help, exit, loadSchema
│       ├── cmd_netconf.go            # set, unset, commit, dump, lock, rpc
│       ├── completer.go             # Tab completion, YANG parser, schema tree
│       └── formatter.go              # XML→text, text→XML, dump formats
├── test/
│   ├── yang/vht-system.yang          # YANG model mẫu
│   ├── mock-netconf/main.go          # Mock NETCONF server (get-schema support)
│   ├── mock-mgt/main.go             # Mock mgt-service API
│   ├── e2e_test.go                   # 28 tests (all-in-one)
│   └── run.sh                        # Script chạy mock servers
├── deploy/k8s.yaml                   # K8s Deployment + Service
├── Dockerfile                        # Multi-stage build (~11MB)
├── Makefile
├── mgt-service.yaml                  # OpenAPI spec của mgt-service
└── go.mod
```

## Cài đặt & Chạy

### Build

```bash
make build
```

### Cấu hình

| Biến | Mặc định | Mô tả |
|---|---|---|
| `SSH_ADDR` | `:2222` | Địa chỉ SSH server |
| `SSH_HOST_KEY_PATH` | _(tự tạo)_ | SSH host key (ED25519) |
| `MGT_SERVICE_URL` | `http://mgt-service:3000` | URL mgt-service API |
| `NETCONF_USERNAME` | `admin` | Account kết nối NETCONF |
| `NETCONF_PASSWORD` | `admin` | Mật khẩu NETCONF |
| `NETCONF_TIMEOUT` | `30s` | Timeout kết nối |

### Chạy local

```bash
./test/run.sh
# Hoặc
MGT_SERVICE_URL=http://127.0.0.1:3000 ./bin/cli-netconf
```

### Kết nối

```bash
ssh admin@127.0.0.1 -p 2222
```

## Cách sử dụng

### Login

```
============================================
        VHT CLI - NETCONF Console
============================================

  #  NE          Site  IP         Port  Namespace  Description
  1  ne-amf-01   HCM   10.0.1.10  830  5gc-hcm    AMF Node
  2  ne-smf-01   HNI   10.0.1.20  830  5gc-hni    SMF Node

Select NE [1-2 or name]: ne-amf-01
Connected. NETCONF session ID: 42
```

### Xem config

```
admin[ne-amf-01]> show running-config system ntp
system ntp
  enabled               true
  server 10.0.0.1
    prefer              true
  server 10.0.0.2
    prefer              false

admin[ne-amf-01]> show running-config system hostname
system hostname                 ne-amf-01
```

### Set config

```
# Inline
admin[ne-amf-01]> set system hostname new-name
OK

# Paste text format
admin[ne-amf-01]> set
Enter config (XML or text format, end with '.' on a new line):
interfaces interface
  name                     veth0
  description              Virtual Interface 0
  enabled                  true
  mtu                      1500
  ipv4
    address                  10.0.0.0
    prefix-length            24
.
OK

admin[ne-amf-01]> commit
Commit successful.
```

### Xoá config

```
admin[ne-amf-01]> unset system contact
OK
admin[ne-amf-01]> commit
Commit successful.
```

### Export

```
admin[ne-amf-01]> dump text /tmp/config.txt
Saved to /tmp/config.txt (259351 bytes)

admin[ne-amf-01]> dump xml /tmp/config.xml
Saved to /tmp/config.xml (289471 bytes)
```

### Tab completion

```
admin[ne-amf-01]> sh<TAB>                          → show
admin[ne-amf-01]> show running-config system <TAB>
  contact    dns        hostname   location   logging    ntp
admin[ne-amf-01]> set system host<TAB>              → set system hostname
admin[ne-amf-01]> unset sys<TAB>                    → unset system
```

## Deploy K8s

```bash
# Secrets
ssh-keygen -t ed25519 -f host_key -N ""
kubectl create secret generic cli-netconf-host-key --from-file=host_key
kubectl create secret generic cli-netconf-secret \
  --from-literal=netconf-username=admin \
  --from-literal=netconf-password=<password>

# Deploy
make docker-build
kubectl apply -f deploy/k8s.yaml
```

## Testing

```bash
# Build + start mock servers
make build
go build -o bin/mock-mgt ./test/mock-mgt
go build -o bin/mock-netconf ./test/mock-netconf
./bin/mock-mgt & ./bin/mock-netconf &
MGT_SERVICE_URL=http://127.0.0.1:3000 ./bin/cli-netconf &

# Run 28 tests
go test -v -timeout 300s ./test/
```

### Test cases (28 tests)

| # | Test | Mô tả |
|---|---|---|
| 1 | WelcomeBannerAndNEList | Banner + auto NE list |
| 2 | ConnectByName | Kết nối NE bằng tên + validate input |
| 3 | ShowNE | Danh sách NE |
| 4 | ShowRunningConfig | Xem toàn bộ config |
| 5 | ShowRunningConfigPathHeader | Full path prefix (system hostname ...) |
| 6 | SetInline | set system hostname value |
| 7 | SetXML | Paste XML config |
| 8 | SetTextConfigPaste | Paste text config format → auto convert XML |
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
| 22 | TabCompleteMultipleOptions | Hiển thị nhiều options |
| 23 | Help | Help text |
| 24 | UnknownCommand | Xử lý lệnh sai |
| 25 | Stress500Configs | Set 500 interfaces (127KB) |
| 26 | Stress1000Configs | Set 1000 interfaces (253KB) |
| 27 | MultiSession | 5 SSH sessions đồng thời |

## Tổng hợp tất cả tính năng đã phát triển

1. **SSH server** với password auth qua mgt-service REST API (JWT)
2. **Auto NE selection** khi login — hiển thị danh sách, chọn bằng số hoặc tên, validate input
3. **NETCONF client** thuần Go — SSH subsystem, hello exchange, RPC send/receive
4. **show running-config / candidate-config** với path filter + full path header
5. **Text output format** — indent, list key display, không raw XML
6. **set** — 3 mode: inline path+value, paste XML, paste text config (auto-detect)
7. **unset** — xoá config node via NETCONF operation="delete"
8. **commit / validate / discard** — candidate datastore workflow
9. **lock / unlock** — datastore locking
10. **dump text / xml** — export config ra file hoặc terminal
11. **Tab completion** — commands, sub-commands, config paths (từ YANG + running config)
12. **YANG schema loading** — get-schema (RFC 6022) + merge running config
13. **Graceful shutdown** — SIGINT/SIGTERM → đóng active sessions → thoát sạch
14. **Multi-session** — goroutine per session, test 5 concurrent OK
15. **Stress tested** — 1000 configs (253KB), show 230KB output, không crash/treo
16. **Optimized NETCONF read** — 64KB buffer, tail-only delimiter search
17. **Mock servers** — mgt-service + NETCONF/ConfD với get-schema support
18. **28 automated tests** — e2e, tab completion, stress, multi-session
19. **K8s deployment** — Dockerfile (~11MB), k8s.yaml, secrets
20. **Module** — `github.com/DoTuanAnh2k1/cli-netconf`, Go 1.25
