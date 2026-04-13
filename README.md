# CLI - NETCONF Console

CLI cho phép user SSH vào để quản lý cấu hình các Network Element (NE) trong hệ thống 5GC thông qua giao thức NETCONF (RFC 6241).

## Kiến trúc

```
User --SSH--> [CLI Pod :2222]
                |
                |-- Auth -----> [mgt-service :3000]  (REST API, JWT)
                |-- List NE --> [mgt-service :3000]
                |-- History --> [mgt-service :3000]
                |-- Backup ---> [mgt-service :3000]  (lưu/load config snapshots)
                |
                |-- NETCONF/SSH --> [NE Pod :830]  (ConfD, YANG model)
```

- **CLI Pod**: SSH server, xác thực user qua mgt-service, cung cấp interactive shell
- **mgt-service**: REST API quản lý user, phân quyền, danh sách NE, lưu trữ config backup
- **NE Pod**: Network Element chạy ConfD với NETCONF subsystem

## Tính năng

### Xác thực & Chọn NE
- SSH vào CLI bằng username/password
- Xác thực qua mgt-service API (`POST /aa/authenticate`)
- Khi login tự động hiển thị danh sách NE và yêu cầu chọn (bằng số hoặc tên)
- Nhập sai tên NE sẽ báo lỗi và yêu cầu nhập lại
- Hỗ trợ multi-session: nhiều user SSH đồng thời, mỗi session độc lập

### Điều hướng session

| Tình huống | Ctrl+C | `exit` |
|---|---|---|
| Đang gõ lệnh (kết nối NE) | Huỷ dòng đang gõ, hiện lại prompt | Ngắt kết nối → quay về chọn NE |
| Màn hình chọn NE | Huỷ dòng đang gõ, hiện lại prompt | Thoát khỏi SSH hoàn toàn |
| Đang không kết nối NE | Huỷ dòng đang gõ | Thoát khỏi SSH hoàn toàn |

Lệnh `disconnect` cũng ngắt kết nối NE và quay về màn hình chọn NE.

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
- Tab complete cả leaf lẫn container: `show running-config system <TAB>` → `contact  dns  hostname  location  logging  ntp`
- Tab complete cho `set`, `unset`: `set system host<TAB>` → `hostname`
- Tab complete cho `dump`: `dump t<TAB>` → `text`
- Schema tự động load từ YANG (get-schema RFC 6022) + running config
- Thấy được tất cả leaf/container kể cả chưa set giá trị

### Command History
- Bấm **↑ (mũi tên lên)** để xem lệnh trước
- Bấm **↓ (mũi tên xuống)** để trở lại lệnh sau
- Lưu toàn bộ lịch sử trong phiên làm việc hiện tại (giữ 100 lệnh)
- Ctrl-R để tìm kiếm ngược trong history (nếu terminal hỗ trợ)

### Pager (cuộn trang)

Khi output của `show running-config` hoặc `show candidate-config` dài hơn 20 dòng, CLI tự động hiển thị từng trang:

```
system
  hostname                 ne-amf-01
  ...                      (dòng 1-20)
<MORE> — 35 lines left  [Enter] next  [a/G/End/PgDn] all  [q] quit
```

| Phím | Hành động |
|---|---|
| **Enter** hoặc **Space** | Hiện trang tiếp theo (20 dòng) |
| **a** hoặc **G** | Hiện toàn bộ phần còn lại ngay lập tức |
| **End** hoặc **PageDown** | Hiện toàn bộ phần còn lại ngay lập tức |
| **q** hoặc **Esc** | Thoát pager, quay lại prompt |

Dòng `<MORE>` tự động xoá khi tiếp tục, không để lại dấu vết trong output.

### Backup & Restore config

Mỗi lần `commit` thành công, CLI tự động chụp lại running config và lưu lên mgt-service. Khi connect lại NE, danh sách backup cũ sẽ được tải về tự động.

| Lệnh | Mô tả |
|---|---|
| `show backups` | Liệt kê tất cả snapshot với ID, thời gian, kích thước |
| `restore <id>` | Roll back về snapshot đó (copy-config → commit) |

```
admin[ne-amf-01]> show backups
  ID  Timestamp              Size       Source
   1  2026-04-13 09:00:00   -          remote #7
   2  2026-04-13 10:15:32   48230 B    saved #8
   3  2026-04-13 11:02:11   49105 B    local

admin[ne-amf-01]> restore 2
Restore to snapshot #2 (2026-04-13 10:15:32)? [y/N] y
Restoring...
Restored to snapshot #2.
```

**Lưu ý:**
- Backup lưu trên mgt-service → tồn tại qua nhiều phiên làm việc
- Nếu mgt-service không khả dụng, backup vẫn lưu tạm trong session hiện tại (không bị cản trở commit)
- Backup từ session cũ hiện dưới dạng `remote #<id>`, XML tải về lazily chỉ khi cần restore
- `restore` bản thân cũng tạo một snapshot mới (để có thể undo nếu nhầm)
- Tab completion cho ID: `restore <TAB>` gợi ý các ID đang có

### Graceful Shutdown
- Server nhận SIGINT/SIGTERM → đóng tất cả active SSH sessions → thoát sạch
- Timeout 10s cho shutdown, không bị treo nếu có user đang kết nối

### Lịch sử & Logging
- Tự động lưu command history qua mgt-service (`POST /aa/history/save`)
- Hiển thị thời gian thực thi mỗi lệnh

## Cấu trúc project

```
cli-netconf/
├── cmd/
│   ├── netconf/main.go               # SSH server + mgt-service auth (production)
│   ├── direct/main.go                # Kết nối trực tiếp ConfD (TCP hoặc SSH, env NETCONF_MODE)
│   ├── direct-ssh/main.go            # Kết nối thẳng ConfD qua SSH (172.16.25.131:2075)
│   └── direct-tcp/main.go            # Kết nối thẳng ConfD qua TCP (127.0.0.1:2023)
├── pkg/
│   ├── config/config.go              # Cấu hình từ env vars
│   ├── api/client.go                 # HTTP client cho mgt-service API
│   ├── netconf/client.go             # NETCONF client — SSH (RFC 6241) và TCP
│   └── server/
│       ├── server.go                 # SSH server, graceful shutdown
│       ├── session.go                # Interactive shell, session lifecycle, configBackup struct
│       ├── direct.go                 # RunDirect — chạy session trực tiếp trên stdio
│       ├── cmd_general.go            # show, connect, disconnect, help, exit, loadSchema
│       ├── cmd_netconf.go            # set, unset, commit, dump, lock, rpc, backup/restore
│       ├── completer.go              # Tab completion, YANG parser, schema tree
│       └── formatter.go              # XML→text, text→XML, dump formats, textConfigToXML
├── test/
│   ├── yang/ne-system.yang           # YANG model mẫu
│   ├── mock-netconf/main.go          # Mock NETCONF server (SSH :8830 + TCP :2023)
│   ├── mock-mgt/main.go              # Mock mgt-service API
│   ├── e2e_test.go                   # 28 tests (all-in-one)
│   └── run.sh                        # Script chạy mock servers
├── deploy/k8s.yaml                   # K8s Deployment + Service
├── Dockerfile                        # Multi-stage build (private registry)
├── Makefile
├── mgt-service.yaml                  # OpenAPI spec của mgt-service
└── go.mod
```

## Cài đặt & Chạy

### Build

```bash
# SSH server (production)
go build -o bin/cli-netconf ./cmd/netconf

# Direct mode — tự chọn TCP hoặc SSH qua env NETCONF_MODE
go build -o bin/cli-direct ./cmd/direct

# Direct SSH — kết nối thẳng qua SSH (hardcode 172.16.25.131:2075)
go build -o bin/cli-direct-ssh ./cmd/direct-ssh

# Direct TCP — kết nối thẳng qua TCP (hardcode 127.0.0.1:2023)
go build -o bin/cli-direct-tcp ./cmd/direct-tcp
```

### Cấu hình (`cmd/netconf`)

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
# Với mock servers
./test/run.sh

# Hoặc manual
MGT_SERVICE_URL=http://127.0.0.1:3000 ./bin/cli-netconf
```

### Direct mode — kết nối thẳng ConfD qua TCP

Chỉnh địa chỉ ConfD trong [cmd/direct/main.go](cmd/direct/main.go) rồi chạy:

```bash
go run ./cmd/direct
```

Bỏ qua hoàn toàn SSH server và mgt-service, kết nối thẳng vào ConfD qua **NETCONF over TCP** (không cần username/password). ConfD cần bật TCP transport trong `confd.conf`:

```xml
<netconf>
  <transport>
    <tcp>
      <enabled>true</enabled>
      <ip>127.0.0.1</ip>
      <port>2023</port>
    </tcp>
  </transport>
</netconf>
```

Môi trường test dùng mock server: `go run ./test/mock-netconf` — tự động lắng nghe cả SSH `:8830` và TCP `:2023`.

### Kết nối (SSH server mode)

```bash
ssh admin@127.0.0.1 -p 2222
```

## Cách sử dụng

### Login

```
============================================
        CLI - NETCONF Console
============================================

  #  NE          Site  IP         Port  Namespace  Description
  1  ne-amf-01   HCM   10.0.1.10  830  5gc-hcm    AMF Node
  2  ne-smf-01   HNI   10.0.1.20  830  5gc-hni    SMF Node

Select NE [1-2 or name] (exit to quit): ne-amf-01
Connected. NETCONF session ID: 42
```

Sau khi kết nối xong, gõ `exit` để quay về màn hình chọn NE, gõ `exit` lần nữa để thoát SSH.

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

### Backup & Restore

```
# Xem danh sách backup (tự động load từ mgt-service khi connect)
admin[ne-amf-01]> show backups
  ID  Timestamp              Size       Source
   1  2026-04-13 09:00:00   -          remote #7
   2  2026-04-13 10:15:32   48230 B    saved #8
   3  2026-04-13 11:02:11   49105 B    local

# Roll back về snapshot #2
admin[ne-amf-01]> restore 2
Restore to snapshot #2 (2026-04-13 10:15:32)? [y/N] y
Restoring...
Restored to snapshot #2.
```

### Tab completion & command history

```
admin[ne-amf-01]> sh<TAB>                              → show
admin[ne-amf-01]> show running-config system <TAB>
  contact    dns        hostname   location   logging    ntp
admin[ne-amf-01]> show running-config system ntp <TAB>
  enabled    server
admin[ne-amf-01]> set system host<TAB>                 → set system hostname
admin[ne-amf-01]> unset sys<TAB>                       → unset system

# Dùng mũi tên ↑/↓ để điều hướng lịch sử lệnh
admin[ne-amf-01]> ↑                                    → lệnh trước đó
```

### Pager

```
admin[ne-amf-01]> show running-config
system
  hostname                 ne-amf-01
  location                 HCM Data Center, Rack A3
  ...
<MORE> — 35 lines left  [Enter] next  [a/G/End/PgDn] all  [q] quit
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
docker build -t cli-netconf .
kubectl apply -f deploy/k8s.yaml
```

## Testing

```bash
# Build
go build -o bin/cli-netconf  ./cmd/netconf
go build -o bin/mock-mgt     ./test/mock-mgt
go build -o bin/mock-netconf ./test/mock-netconf

# Start mock servers
./bin/mock-mgt &      # mgt-service tại :3000
./bin/mock-netconf &  # NETCONF SSH tại :8830, TCP tại :2023

# Start CLI server
MGT_SERVICE_URL=http://127.0.0.1:3000 ./bin/cli-netconf &

# Run 28 e2e tests
go test -v -timeout 300s ./test/
```

### Test direct mode (TCP)

```bash
go run ./cmd/direct   # kết nối TCP vào mock-netconf :2023, không cần auth
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
