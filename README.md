# CLI - NETCONF Console

CLI quản lý cấu hình Network Element (NE) trong hệ thống 5GC qua giao thức NETCONF (RFC 6241).

Có hai implementation:
- **`go/`** — Go: SSH server (production) + direct mode
- **`c/`** — C: direct mode, tab completion YANG-aware, hỗ trợ MAAPI (ConfD)

## Kiến trúc

```
User ──SSH──► [CLI Pod :2222]           (Go SSH server)
                │
                ├── Auth ────────► [mgt-service :3000]  (JWT)
                ├── List NE ─────► [mgt-service :3000]
                ├── History ─────► [mgt-service :3000]  (lưu command log)
                ├── Backup ──────► [mgt-service :3000]  (config snapshot)
                │
                └── NETCONF/SSH ─► [NE :830]            (ConfD)

User ──stdin─► [cli-netconf-c]          (C full mode)
                │
                ├── Auth ────────► [mgt-service :3000]
                ├── NETCONF/TCP ─► [NE :2023]
                ├── NETCONF/SSH ─► [NE :830]            (WITH_SSH=1)
                └── MAAPI ───────► [ConfD :4565]        (WITH_MAAPI=1)

User ──stdin─► [cli-netconf-c-direct]   (C direct mode — không cần mgt-service)
                └── NETCONF/TCP|SSH ──► [NE]

User ──stdin─► [cli-netconf-c-maapi]    (C MAAPI mode — không cần NETCONF)
                └── MAAPI IPC ────────► [ConfD :4565]
```

## Cấu trúc project

```
cli-netconf/
├── go/
│   ├── cmd/
│   │   ├── netconf/main.go       # SSH server (production)
│   │   ├── direct/main.go        # Direct mode (env NETCONF_MODE)
│   │   ├── direct-ssh/main.go    # Direct SSH
│   │   └── direct-tcp/main.go    # Direct TCP
│   ├── pkg/
│   │   ├── api/client.go         # mgt-service HTTP client
│   │   ├── config/config.go      # Cấu hình từ env vars
│   │   ├── netconf/client.go     # NETCONF client (SSH + TCP)
│   │   └── server/
│   │       ├── server.go         # SSH server
│   │       ├── session.go        # Shell session, backup
│   │       ├── direct.go         # RunDirect — stdio session
│   │       ├── cmd_general.go    # show, connect, disconnect, help
│   │       ├── cmd_netconf.go    # set, commit, dump, lock, rpc, restore
│   │       ├── completer.go      # Tab completion, YANG parser
│   │       └── formatter.go      # XML↔text
│   ├── pkg/ccli/
│   │   └── ccli.go               # CGo wrapper — gọi C lib từ Go
│   ├── test/
│   │   ├── mock-mgt/main.go      # Mock mgt-service (:3000)
│   │   ├── mock-netconf/main.go  # Mock NETCONF (:8830 SSH / :2023 TCP)
│   │   ├── e2e_test.go           # 27 e2e tests
│   │   └── run.sh                # Khởi động tất cả services
│   ├── deploy/k8s.yaml
│   ├── Dockerfile
│   ├── Makefile
│   └── go.mod
│
├── c/
│   ├── include/
│   │   ├── cli.h                 # Internal types & declarations
│   │   ├── libclinetconf.h       # Public library API
│   │   └── maapi-direct.h        # MAAPI session types
│   ├── src/
│   │   ├── main.c                # Full mode entry point
│   │   ├── main-direct.c         # Direct TCP/SSH entry point
│   │   ├── main-maapi.c          # MAAPI direct entry point
│   │   ├── lib.c                 # libclinetconf implementation
│   │   ├── auth.c                # mgt-service REST API (libcurl)
│   │   ├── auth-stub.c           # No-op stubs cho direct/maapi mode
│   │   ├── netconf.c             # NETCONF client (TCP + SSH)
│   │   ├── schema.c              # YANG schema tree
│   │   ├── maapi.c               # MAAPI schema provider
│   │   ├── maapi-ops.c           # MAAPI config operations
│   │   ├── completer.c           # GNU readline tab completion
│   │   ├── formatter.c           # XML↔text, pager
│   │   └── commands.c            # Command handlers
│   └── Makefile
│
├── local-test.sh                 # Script test local
├── Makefile                      # Root Makefile
└── README.md
```

---

## Cài đặt & Build

### Yêu cầu

**Go:** Go 1.21+

**C (macOS):**
```bash
brew install readline libssh2   # libssh2 tuỳ chọn
```

**C (Linux):**
```bash
apt install libcurl4-openssl-dev libxml2-dev libreadline-dev libssh2-1-dev
```

**MAAPI mode:** cần ConfD SDK (`CONFD_DIR=/opt/confd`)

### Build

```bash
# Build tất cả
make all

# Chỉ Go
make build-go        # → go/bin/cli-netconf, go/bin/cli-direct

# Chỉ C (full mode)
make build-c         # → c/cli-netconf-c

# C direct mode (không cần mgt-service)
make build-c-direct  # → c/cli-netconf-c-direct
make -C c WITH_SSH=1 direct   # có SSH support

# C MAAPI mode (không cần NETCONF)
make -C c WITH_MAAPI=1 CONFD_DIR=/opt/confd maapi-direct
# → c/cli-netconf-c-maapi

# C library (để dùng từ Go CGo hoặc C khác)
make -C c lib         # → c/libclinetconf.a (static)
make -C c lib-shared  # → c/libclinetconf.so (shared)
```

---

## Chạy local (test với mock servers)

```bash
bash local-test.sh
```

Hiện menu:
```
  1  Go direct mode   ← nhanh nhất, không cần SSH client
  2  C full mode      (mgt-service + NETCONF TCP)
  3  C direct mode    (chỉ cần NETCONF server)
  4  Go SSH server    (ssh admin@127.0.0.1 -p 2222)
```

Hoặc chỉ định thẳng:
```bash
bash local-test.sh go-direct
bash local-test.sh c-direct
bash local-test.sh c
bash local-test.sh go-ssh
```

Thủ công:
```bash
# Terminal 1 — mock servers
cd go
./bin/mock-mgt &          # mgt-service :3000
./bin/mock-netconf &      # NETCONF SSH:8830 / TCP:2023

# Terminal 2 — CLI
NETCONF_HOST=127.0.0.1 NETCONF_PORT=2023 NETCONF_MODE=tcp \
  ./c/cli-netconf-c-direct
```

---

## Hướng dẫn sử dụng

### Khởi động

**Go SSH server:**
```
ssh admin@<host> -p 2222
password: admin
```

**C full mode:**
```bash
MGT_URL=http://mgt-service:3000 ./c/cli-netconf-c
# → nhập username / password
# → chọn NE
```

**C direct mode** (không cần mgt-service):
```bash
NETCONF_HOST=10.0.1.10 NETCONF_PORT=830 NETCONF_MODE=ssh \
NETCONF_USER=admin NETCONF_PASS=admin NE_NAME=ne-amf-01 \
./c/cli-netconf-c-direct
```

**C MAAPI mode** (kết nối thẳng ConfD):
```bash
CONFD_IPC_ADDR=10.0.1.10 CONFD_IPC_PORT=4565 NE_NAME=ne-amf-01 \
./c/cli-netconf-c-maapi
```

---

### Màn hình chào

```
============================================
        CLI - NETCONF Console
============================================

  #  NE          Site  IP          Port  Description
  1  ne-amf-01   HCM   10.0.1.10   830   AMF Node
  2  ne-smf-01   HNI   10.0.1.20   830   SMF Node

Select NE [1-2 or name] (exit to quit): 1
Connecting to ne-amf-01 (10.0.1.10:830) via ssh...
Connected. NETCONF session ID: 42
Loading schema...

admin[ne-amf-01]>
```

---

### Xem cấu hình

```bash
# Toàn bộ running config
show running-config

# Filter theo path (space-separated)
show running-config system
show running-config system ntp
show running-config system ntp server

# Candidate config
show candidate-config
show candidate-config system hostname
```

Output dạng text indent:
```
system
  hostname                 ne-amf-01
  location                 HCM Data Center
  contact                  noc@5gc.local
  ntp
    enabled                true
    server 10.0.0.1
      address              10.0.0.1
      prefer               true
```

Output dài hơn 20 dòng tự động phân trang:

| Phím | Hành động |
|---|---|
| Enter | Trang tiếp |
| `a` / `G` | Hiện toàn bộ |
| `q` | Thoát pager |

---

### Thiết lập cấu hình

**Set một giá trị:**
```bash
set system hostname new-name
set system ntp enabled true
set system ntp server 10.0.0.1 prefer true   # list key tự động xử lý
```

**Set nhiều giá trị cùng lúc** (paste mode):
```
set
Enter config (XML or text format, end with '.' on a new line):
system
  hostname new-name
  location HCM DC Rack-B
  ntp
    enabled true
.
OK
```

Hoặc paste XML trực tiếp:
```
set
<system xmlns="urn:5gc:system">
  <hostname>new-name</hostname>
  <ntp><enabled>true</enabled></ntp>
</system>
.
OK
```

**Xoá config:**
```bash
unset system ntp server 10.0.0.1
unset system contact
```

**Commit / Validate / Discard:**
```bash
validate          # kiểm tra candidate trước khi commit
commit            # áp dụng candidate → running
discard           # huỷ toàn bộ thay đổi candidate
```

**Lock / Unlock:**
```bash
lock              # lock candidate (default)
lock running
unlock
unlock running
```

---

### Export config

```bash
# In ra terminal (có pager)
dump text
dump xml

# Chọn datastore
dump text candidate
dump xml running

# Ghi ra file
dump text /tmp/config.txt
dump xml running /tmp/running.xml
dump text candidate /tmp/candidate.txt
```

---

### Backup & Restore

Mỗi `commit` thành công tự động lưu snapshot lên mgt-service.

```bash
show backups
```
```
  ID  Timestamp              Size     Source
   1  2026-04-14 09:00:00    local    remote
   2  2026-04-14 10:30:00    local    remote
```

```bash
restore 1          # Roll back về snapshot #1 (có xác nhận y/N)
```

---

### Tab Completion

Completion gợi ý đầy đủ khi có nhiều lựa chọn:

```
> show <TAB>
backups  candidate-config  ne  running-config

> show running-config system <TAB>
contact  dns  hostname  location  logging  ntp

> set system ntp <TAB>
enabled  server  timezone

> lock <TAB>
candidate  running
```

- Tab 1 lần: complete nếu unique, hiện list nếu có nhiều options
- Schema load từ YANG (`get-schema` RFC 6022) + XML fallback + MAAPI

---

### Điều hướng session

| Tình huống | `Ctrl+C` | `exit` |
|---|---|---|
| Đang gõ lệnh (đã kết nối NE) | Huỷ dòng | Ngắt kết nối → về chọn NE |
| Màn hình chọn NE | Huỷ dòng | Thoát hoàn toàn |

```bash
disconnect          # ngắt kết nối NE, quay về màn hình chọn NE
connect ne-smf-01   # kết nối NE khác (không cần disconnect trước)
```

---

### Raw RPC

Gửi NETCONF RPC tự do:
```
rpc
<get-schema xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring">
  <identifier>ietf-interfaces</identifier>
</get-schema>
.
```

---

### MAAPI mode — commands đặc thù

MAAPI mode dùng keypath ConfD:

```bash
# Set theo path thường (tự convert qua schema)
set system hostname new-name
set system ntp server 10.0.0.1 prefer true

# Hoặc keypath ConfD trực tiếp (bắt đầu bằng /)
set /system/hostname new-name
unset /system/ntp/server{10.0.0.1}

# Dump
dump text          # running config
dump xml candidate # candidate config
```

---

## Cấu hình qua env vars

### Go SSH server

| Biến | Mặc định | Mô tả |
|---|---|---|
| `SSH_ADDR` | `:2222` | SSH server listen address |
| `MGT_SERVICE_URL` | `http://mgt-service:3000` | mgt-service URL |
| `NETCONF_USERNAME` | `admin` | NETCONF username |
| `NETCONF_PASSWORD` | `admin` | NETCONF password |
| `NETCONF_TIMEOUT` | `30s` | Connection timeout |

### Go / C direct mode

| Biến | Mặc định | Mô tả |
|---|---|---|
| `NETCONF_HOST` | _(từ NE list)_ | Override host |
| `NETCONF_PORT` | _(từ NE list)_ | Override port |
| `NETCONF_MODE` | `tcp` | `tcp` hoặc `ssh` |
| `NETCONF_USER` | `admin` | SSH username |
| `NETCONF_PASS` | `admin` | SSH password |
| `NE_NAME` | `confd` | Label hiện trong prompt |

### C full mode

| Biến | Mặc định | Mô tả |
|---|---|---|
| `MGT_URL` | `http://127.0.0.1:3000` | mgt-service URL |

### C MAAPI mode

| Biến | Mặc định | Mô tả |
|---|---|---|
| `CONFD_IPC_ADDR` | `127.0.0.1` | ConfD host |
| `CONFD_IPC_PORT` | `4565` | ConfD MAAPI port |
| `MAAPI_USER` | `admin` | MAAPI session user |
| `NE_NAME` | `confd` | Label trong prompt |

---

## C Library (libclinetconf)

Dùng C implementation như một thư viện — không cần chạy binary.

### Build

```bash
make -C c lib         # → c/libclinetconf.a
make -C c lib-shared  # → c/libclinetconf.so
```

### Dùng từ Go (CGo)

```go
import "github.com/DoTuanAnh2k1/cli-netconf/pkg/ccli"

h := ccli.New("http://mgt-service:3000")
defer h.Close()

h.Login("admin", "admin")
h.ListNE()
for _, ne := range h.NEs() {
    fmt.Printf("%s  %s:%d\n", ne.Name, ne.IP, ne.Port)
}

h.ConnectNE("ne-smf-01", ccli.ConnectOpts{Mode: "tcp", Port: 2023})
h.LoadSchema()

cfg, _ := h.GetConfig("running", "")
h.EditConfig("candidate", `<system><hostname>new-host</hostname></system>`)
h.Commit()
```

### Dùng từ C

```c
#include "libclinetconf.h"

cli_handle_t *h = cli_create("http://mgt-service:3000");
cli_login(h, "admin", "admin");
cli_list_ne(h);
cli_connect(h, "ne-smf-01", "tcp", "10.0.1.20", 2023, NULL, NULL);

char *xml = cli_get_config(h, "running", NULL);
cli_edit_config(h, "candidate", "<system><hostname>new</hostname></system>");
cli_commit(h);
free(xml);
cli_destroy(h);
```

```bash
gcc myapp.c -I c/include -L c -lclinetconf \
  $(curl-config --libs) $(xml2-config --libs) -o myapp
```

---

## Testing

```bash
# Go e2e tests (27 tests)
make test-go
# hoặc: cd go && go test -v -timeout 300s ./test/

# Local test interactive
bash local-test.sh
```

---

## Deploy K8s (Go SSH server)

```bash
cd go

# Host key
ssh-keygen -t ed25519 -f host_key -N ""
kubectl create secret generic cli-netconf-host-key --from-file=host_key

# Credentials
kubectl create secret generic cli-netconf-secret \
  --from-literal=netconf-username=admin \
  --from-literal=netconf-password=<password>

# Build & deploy
docker build -t cli-netconf .
kubectl apply -f deploy/k8s.yaml
```

**K8s env vars:**
```yaml
env:
  - name: MGT_SERVICE_URL
    value: "http://mgt-service:3000"
  - name: SSH_ADDR
    value: ":2222"
```
