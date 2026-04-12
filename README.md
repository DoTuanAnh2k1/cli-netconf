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

### Xác thực & Phân quyền
- SSH vào CLI bằng username/password
- Xác thực qua mgt-service API (`POST /aa/authenticate`)
- Chỉ hiển thị các NE mà user được phân quyền (`GET /aa/list/ne`)
- Khi login tự động hiển thị danh sách NE và cho chọn NE để kết nối

### Quản lý Network Element
- Hiển thị danh sách NE (tên, site, IP, port, namespace)
- Kết nối đến NE bằng số thứ tự hoặc tên NE
- Kết nối qua NETCONF over SSH subsystem
- Hỗ trợ multi-session: nhiều user SSH đồng thời, mỗi session độc lập

### Lệnh cấu hình
| Lệnh | Mô tả |
|---|---|
| `show running-config [path...]` | Xem cấu hình running |
| `show candidate-config [path...]` | Xem cấu hình candidate |
| `set` | Thiết lập cấu hình candidate (nhập XML) |
| `commit` | Áp dụng candidate vào running |
| `validate` | Kiểm tra candidate trước khi commit |
| `discard` | Huỷ bỏ thay đổi candidate |
| `lock [datastore]` | Khoá datastore (tránh xung đột) |
| `unlock [datastore]` | Mở khoá datastore |
| `rpc` | Gửi raw NETCONF XML RPC |

### Hiển thị cấu hình dạng text
- Output dạng text có indent, không phải XML raw
- Container hiển thị tên + xuống dòng, leaf hiển thị `tên    giá trị`
- List entry tự động hiển thị key value cạnh tên (ví dụ: `interface eth0`, `server 10.0.0.1`)
- Filter theo path: `show running-config system ntp server` chỉ hiển thị phần được chỉ định

### Tab Completion
- Tab để auto-complete lệnh: `sh<TAB>` → `show`
- Tab để complete sub-command: `show r<TAB>` → `show running-config`
- Tab để duyệt config path: `show running-config system <TAB>` → hiện tất cả children
- Tab lần 2 (khi có nhiều lựa chọn) hiển thị danh sách options
- Schema tree được tự động load từ running config khi kết nối NE

### Lịch sử & Logging
- Tự động lưu command history qua mgt-service (`POST /aa/history/save`)
- Hiển thị thời gian thực thi mỗi lệnh
- Server log: auth events, connections

### Khác
- ANSI color output (prompt, error, success)
- Multiline XML input cho lệnh `set` và `rpc` (kết thúc bằng `.` trên dòng riêng)
- Timeout cho mỗi NETCONF operation (tránh treo)
- Đã test với 1000 config entries (~289KB XML) - không crash, không treo

## Cấu trúc project

```
cli-netconf/
├── cmd/cli-netconf/main.go           # Entry point, khởi tạo SSH server
├── internal/
│   ├── config/config.go              # Đọc cấu hình từ env vars
│   ├── api/client.go                 # HTTP client cho mgt-service API
│   ├── netconf/client.go             # NETCONF over SSH client (RFC 6241)
│   └── server/
│       ├── server.go                 # SSH server, password auth
│       ├── session.go                # Interactive shell, session lifecycle
│       ├── cmd_general.go            # Lệnh: show, connect, help, exit...
│       ├── cmd_netconf.go            # Lệnh: set, commit, validate, lock...
│       ├── completer.go              # Tab completion + schema tree
│       └── formatter.go              # XML → text formatter
├── test/
│   ├── yang/vht-system.yang          # YANG model mẫu (system, interfaces)
│   ├── mock-netconf/main.go          # Mock NETCONF server để test
│   ├── mock-mgt/main.go             # Mock mgt-service API để test
│   ├── e2e_test.go                   # E2E tests
│   ├── completion_test.go            # Tab completion tests
│   ├── stress_test.go                # Stress tests (500-1000 configs, multi-session)
│   └── run.sh                        # Script chạy tất cả mock + CLI
├── deploy/k8s.yaml                   # K8s Deployment + Service
├── Dockerfile                        # Multi-stage build (~11MB image)
├── Makefile                          # Build, run, docker
├── mgt-service.yaml                  # OpenAPI spec của mgt-service
├── go.mod / go.sum
└── .gitignore
```

## Cài đặt & Chạy

### Yêu cầu
- Go 1.22+

### Build

```bash
make build
# hoặc
go build -o bin/cli-netconf ./cmd/cli-netconf
```

### Cấu hình (Environment Variables)

| Biến | Mặc định | Mô tả |
|---|---|---|
| `SSH_ADDR` | `:2222` | Địa chỉ SSH server lắng nghe |
| `SSH_HOST_KEY_PATH` | _(trống = tự tạo)_ | Đường dẫn SSH host key (ED25519) |
| `MGT_SERVICE_URL` | `http://mgt-service:3000` | URL của mgt-service API |
| `NETCONF_USERNAME` | `admin` | Service account để kết nối NETCONF đến NE |
| `NETCONF_PASSWORD` | `admin` | Mật khẩu service account |
| `NETCONF_TIMEOUT` | `30s` | Timeout kết nối NETCONF |

### Chạy local (dev)

```bash
# Chạy với mock servers (mgt-service + NETCONF)
./test/run.sh

# Hoặc chạy từng thành phần
MGT_SERVICE_URL=http://127.0.0.1:3000 ./bin/cli-netconf
```

### Kết nối

```bash
ssh admin@127.0.0.1 -p 2222
# Mật khẩu: admin
```

## Cách sử dụng

### Login và chọn NE

Khi SSH vào, CLI tự động hiển thị danh sách NE và yêu cầu chọn:

```
============================================
        VHT CLI - NETCONF Console
============================================

  #  NE          Site  IP         Port  Namespace  Description
  1  ne-amf-01   HCM   10.0.1.10  830  5gc-hcm    AMF Node
  2  ne-smf-01   HNI   10.0.1.20  830  5gc-hni    SMF Node

Select NE [1-2 or name]: ne-amf-01
Connecting to ne-amf-01 (10.0.1.10:830)...
Connected. NETCONF session ID: 42
```

Có thể nhập số thứ tự hoặc tên NE. Nhập sai sẽ yêu cầu nhập lại.

### Xem cấu hình

```
admin[ne-amf-01]> show running-config
system
  hostname                ne-amf-01
  location                HCM Data Center
  contact                 noc@vht.com.vn
  ntp
    enabled               true
    server 10.0.0.1
      prefer              true
    server 10.0.0.2
      prefer              false
  dns
    search                vht.internal
    search                vht.com.vn
    server 8.8.8.8
    server 8.8.4.4
interfaces
  interface eth0
    description           Management Interface
    enabled               true
    mtu                   1500
    ipv4
      address             10.0.1.10
      prefix-length       24
      gateway             10.0.1.1
```

### Filter theo path

```
admin[ne-amf-01]> show running-config system ntp
ntp
  enabled               true
  server 10.0.0.1
    prefer              true
  server 10.0.0.2
    prefer              false

admin[ne-amf-01]> show running-config system contact
contact                 noc@vht.com.vn
```

### Thiết lập cấu hình

```
admin[ne-amf-01]> set
Enter config XML (end with '.' on a new line):
<system xmlns="urn:vht:params:xml:ns:yang:vht-system">
  <hostname>ne-amf-01-updated</hostname>
</system>
.
OK

admin[ne-amf-01]> commit
Commit successful.
```

### Lock/Unlock datastore

```
admin[ne-amf-01]> lock
Locked candidate.

admin[ne-amf-01]> set
...
admin[ne-amf-01]> commit
Commit successful.

admin[ne-amf-01]> unlock
Unlocked candidate.
```

### Gửi raw NETCONF RPC

```
admin[ne-amf-01]> rpc
Enter RPC body XML (end with '.' on a new line):
<get-config>
  <source><running/></source>
</get-config>
.
```

### Tab completion

```
admin[ne-amf-01]> sh<TAB>                     → show
admin[ne-amf-01]> show r<TAB>                  → show running-config
admin[ne-amf-01]> show running-config sys<TAB> → show running-config system
admin[ne-amf-01]> show running-config system <TAB><TAB>
  contact    dns        hostname   location   logging    ntp
```

### Danh sách lệnh

```
admin[ne-amf-01]> help
```

## Deploy lên K8s

### 1. Tạo secrets

```bash
ssh-keygen -t ed25519 -f host_key -N ""
kubectl create secret generic cli-netconf-host-key --from-file=host_key

kubectl create secret generic cli-netconf-secret \
  --from-literal=netconf-username=admin \
  --from-literal=netconf-password=<password>
```

### 2. Build & push image

```bash
make docker-build
docker tag cli-netconf <registry>/cli-netconf:latest
docker push <registry>/cli-netconf:latest
```

### 3. Deploy

```bash
kubectl apply -f deploy/k8s.yaml
```

### 4. Truy cập

```bash
kubectl get svc cli-netconf
ssh admin@<node-ip> -p <node-port>
```

## Testing

### Chạy tất cả tests

```bash
# Build mock servers
go build -o bin/mock-mgt ./test/mock-mgt
go build -o bin/mock-netconf ./test/mock-netconf
go build -o bin/cli-netconf ./cmd/cli-netconf

# Khởi động
./bin/mock-mgt &
./bin/mock-netconf &
MGT_SERVICE_URL=http://127.0.0.1:3000 ./bin/cli-netconf &

# Chạy tests
go test -v -timeout 300s ./test/
```

### Test cases

| Test | Mô tả |
|---|---|
| TestWelcomeBanner | Hiển thị banner khi login |
| TestShowNE | Danh sách NE |
| TestConnectAndGetConfig | Kết nối + lấy config |
| TestEditConfigAndCommit | Sửa + commit config |
| TestLockUnlock | Lock/unlock datastore |
| TestDiscardChanges | Discard changes |
| TestHelp | Hiển thị help |
| TestUnknownCommand | Xử lý lệnh không hợp lệ |
| TestTabCompleteCommand | Tab complete lệnh |
| TestTabCompleteShowNe | Tab complete sub-command |
| TestTabCompleteXpath | Tab complete config path |
| TestTabMultipleCompletions | Hiển thị nhiều options |
| TestStressSet500Configs | Set 500 interfaces (145KB) |
| TestStressSet1000Configs | Set 1000 interfaces (289KB) |
| TestMultiSession | 5 sessions SSH đồng thời |

### Mock servers

| Service | Port | Mô tả |
|---|---|---|
| mock-mgt | 3000 | Mock mgt-service API (users: admin/admin, operator/operator123) |
| mock-netconf | 8830 | Mock NETCONF server với YANG model vht-system |

### YANG model mẫu (vht-system)

Mock NETCONF server sử dụng YANG model `vht-system` với:
- `system`: hostname, location, contact, ntp, dns, logging
- `interfaces`: danh sách interface với ipv4 config

Xem chi tiết tại `test/yang/vht-system.yang`.

## Stress test kết quả

| Test | Config size | Thời gian | Kết quả |
|---|---|---|---|
| Set 500 interfaces | 145KB XML | ~14s | OK, không crash |
| Set 1000 interfaces | 289KB XML | ~18s | OK, không crash |
| Show 1000 interfaces | 334KB output | Instant | OK, không treo |
| 5 sessions đồng thời | - | ~6s | Tất cả OK |

## So sánh với Java + MaAPI

| | Java + MaAPI | Go + NETCONF |
|---|---|---|
| Giao thức | MaAPI (IPC, blocking) | NETCONF over SSH (non-blocking) |
| Concurrency | Thread (nặng, giới hạn) | Goroutine (nhẹ, hàng triệu) |
| Timeout | Không có → treo | Context + deadline mỗi RPC |
| Large config | Treo khi config dài | Test OK với 1000 entries (289KB) |
| Multi-session | Giới hạn thread | Test OK 5 sessions đồng thời |
| Memory | JVM ~200MB+ | ~10-20MB |
| Binary | Cần JRE | Single binary ~11MB |
| K8s image | ~300MB+ | ~15MB (Alpine) |
