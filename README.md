# CLI NETCONF — ConfD MAAPI Console

CLI quản lý cấu hình Network Element (NE) qua ConfD MAAPI (Management Agent API).
Kết nối trực tiếp vào ConfD IPC port — không cần NETCONF session hay SSH tới NE.

Hỗ trợ 2 chế độ chạy:
- **SSH Server mode** (production): user SSH vào → PAM auth qua mgt-svc → chọn NE → CLI
- **Login mode** (standalone): chạy binary → `login` → chọn NE → CLI
- **Direct mode** (debug): set `CONFD_IPC_ADDR` → kết nối thẳng tới ConfD, bỏ qua login

---

## Yêu cầu build

| Thư viện | Ubuntu/Debian | RHEL/AlmaLinux |
|---|---|---|
| gcc | `apt install gcc` | `dnf install gcc` |
| libxml2 | `apt install libxml2-dev` | `dnf install libxml2-devel` |
| readline | `apt install libreadline-dev` | `dnf install readline-devel` |
| libconfd.so | Từ ConfD container hoặc SDK | — |

---

## Build

```bash
# Lấy libconfd.so từ container ConfD đang chạy
docker cp <confd-container>:/usr/lib64/libconfd.so ./libconfd-server.so
docker cp <confd-container>:/usr/lib64/libcrypto.so.10 ./libcrypto-server.so

# Build
make CONFD_LIB=./libconfd-server.so

# Hoặc build bằng Docker (không cần cài lib trên host)
docker build -f Dockerfile.clitest -t cli-netconf:clitest .

# Dọn
make clean
```

---

## Chế độ 1: SSH Server Mode (production)

Chế độ chính khi deploy. User SSH vào container → PAM xác thực qua mgt-svc →
CLI nhận JWT token → hiển thị danh sách NE → user chọn → kết nối MAAPI.

### Flow

```
User ──SSH──→ [sshd] ──PAM──→ POST /aa/authenticate → mgt-svc
               │                    (xác thực + lưu JWT token)
               │
               ▼ (ForceCommand)
         [cli-netconf]
               │
               │  (đọc token từ PAM, skip login)
               ▼
         GET /aa/list/ne → danh sách NE
               │
         #   Site    NE        ConfD IP     Port
         1   HN01    smf-01    172.19.0.2   23645
         2   HN02    smf-02    172.19.0.3   23645
         Select NE: 1
               │
         MAAPI connect → conf_master_ip:conf_port_master_tcp
               │
         maapi[smf-01]> _
```

### Build & chạy Docker

```bash
# Build
docker build -f Dockerfile.full -t cli-netconf-full .

# Chạy
docker run -d -p 2222:22 \
  -v cli-ssh-keys:/etc/ssh/host-keys \
  -e MGT_SVC_BASE=http://mgt-service:8080 \
  -e LOG_LEVEL=info \
  -e SEED_USERNAME=admin \
  -e SEED_PASSWORD=admin123 \
  cli-netconf-full

# SSH vào
ssh admin@localhost -p 2222
```

> **Lưu ý**: Mount volume `-v cli-ssh-keys:/etc/ssh/host-keys` để SSH host key
> persist qua restart, tránh lỗi "REMOTE HOST IDENTIFICATION HAS CHANGED".

### Biến môi trường

| Biến | Mặc định | Mô tả |
|---|---|---|
| `MGT_SVC_BASE` | `http://mgt-service:8080` | URL gốc mgt-svc |
| `LOG_LEVEL` | `info` | `debug` / `info` / `warn` / `error` / `off` |
| `SEED_USERNAME` | `anhdt195` | User để sync danh sách user từ mgt-svc |
| `SEED_PASSWORD` | `123` | Password của seed user |

### Deploy K8s

```bash
kubectl apply -f deploy/k8s.yaml
kubectl get svc cli-netconf    # Lấy NodePort
ssh admin@<node-ip> -p <nodeport>
```

---

## Chế độ 2: Login Mode (standalone)

Chạy binary trực tiếp, không cần SSH server. CLI khởi động không kết nối ConfD,
user dùng lệnh `login` để xác thực → tự động fetch NE list → chọn NE → kết nối.

```bash
MGT_SVC_BASE=http://localhost:9233 LD_LIBRARY_PATH=. ./cli-netconf
```

```
=====================================================
   CLI - NETCONF Console (C / MAAPI Direct Mode)
=====================================================

No CONFD_IPC_ADDR set. Use login to connect via mgt-svc.
Type help for commands.

maapi[confd]> login anhdt195
Password: ****
Logged in as anhdt195

Fetching NE list...

#     Site    NE     Namespace   ConfD IP       Port    Description
────  ──────  ─────  ──────────  ─────────────  ──────  ──────────────────
1     HCM     eir    hteir01     172.19.0.2     23645   EIR - Equipment Identity Register

Select NE (# or namespace): 1

Connecting to eir (172.19.0.2:23645)...
Loading schema...
Schema loaded.
Connected to eir (172.19.0.2:23645)

maapi[eir]> show running-config
...
```

Chọn NE bằng **số thứ tự** hoặc **namespace**. Nhập sai → hỏi lại. Ctrl+D để huỷ.

CLI kết nối tới ConfD qua `conf_master_ip`:`conf_port_master_tcp` từ API response.

---

## Chế độ 3: Direct Mode (debug)

Kết nối thẳng tới ConfD qua MAAPI IPC, bỏ qua login mgt-svc.
Kích hoạt bằng cách set `CONFD_IPC_ADDR` hoặc `CONFD_IPC_PORT`.

```bash
CONFD_IPC_ADDR=172.19.0.2 \
CONFD_IPC_PORT=23645 \
NE_NAME=eir \
LD_LIBRARY_PATH=. \
./cli-netconf
```

### Biến môi trường

| Biến | Mặc định | Mô tả |
|---|---|---|
| `CONFD_IPC_ADDR` | — | Địa chỉ ConfD IPC (set để kích hoạt direct mode) |
| `CONFD_IPC_PORT` | `4565` | Port ConfD IPC |
| `MAAPI_USER` | `admin` | Username cho MAAPI session |
| `NE_NAME` | `confd` | Tên hiển thị trong prompt |

---

## Lệnh

```
show running-config [path...]      Xem cấu hình running
show candidate-config [path...]    Xem cấu hình candidate

set <path...> <value>              Đặt giá trị leaf
set                                Paste XML config (dòng trống để kết thúc)

unset <path...>                    Xoá node

commit                             Commit candidate → running
validate                           Validate candidate
discard                            Reset candidate về running

lock [running|candidate]           Lock datastore
unlock [running|candidate]         Unlock datastore

dump text [file]                   Export config dạng text
dump xml  [file]                   Export config dạng XML

rollback                           List rollback files
rollback <nr> [commit]             Stage rollback (commit = áp dụng luôn)

login <user> [<password>]          Đăng nhập mgt-svc → chọn NE → kết nối
logout                             Xoá token khỏi bộ nhớ
nodes                              Xem lại danh sách NE (cần login trước)

save [--scope=X] <cmd_name>        POST CLI history lên mgt-svc

help                               Danh sách lệnh
exit                               Thoát
```

---

## Path syntax

### Space-separated (khuyến nghị)

```
show running-config eir
set eir nsmfPduSession is-enabled true
unset eir networkConfig
```

### ConfD keypath (legacy)

```
set /eir/nsmfPduSession/is-enabled true
unset /eir/networkConfig
```

---

## Tab completion

- `show <TAB>` → `running-config`, `candidate-config`
- `show running-config <TAB>` → top-level containers (aaa, eir, nacm, ...)
- `set eir nsmfPdu<TAB>` → `nsmfPduSession`
- `dump <TAB>` → `text`, `xml`
- `lock <TAB>` → `running`, `candidate`

Schema tự động load từ ConfD `cs_node` tree — không cần YANG files.

---

## Paste XML

```
maapi[eir]> set
Paste XML config (empty line to finish):
<eir xmlns="http://yang.vht.vn/v5gc/eir/2.0">
  <nsmfPduSession>
    <is-enabled>true</is-enabled>
  </nsmfPduSession>
</eir>

OK (staged in candidate)
maapi[eir]> commit
Commit successful.
```

---

## Logging

Log ghi ra file, không lẫn với stdout/stderr.

| Biến | Mặc định | Mô tả |
|---|---|---|
| `LOG_LEVEL` | `info` | `debug` / `info` / `warn` / `error` / `off` |
| `LOG_FILE` | `/tmp/cli-netconf.log` | Đường dẫn file log |

Trong SSH server mode, log mỗi user ghi riêng: `/var/log/cli-netconf/<username>.log`

---

## Cấu trúc source

```
src/
  main.c          Entry point, CLI loop, mgt-svc auth, NE selection, HTTP client
  maapi-ops.c     MAAPI operations (get/set/commit/lock/rollback)
  maapi.c         MAAPI schema loader (walk ConfD cs_node tree)
  schema.c        Schema tree (build, lookup, YANG/XML parser)
  formatter.c     XML ↔ text converter, RPC reply parser

include/
  cli.h           Types, constants, forward declarations
  log.h           Macro-based logging (zero-cost khi tắt)
  maapi-direct.h  MAAPI session type, function declarations
  confd_compat.h  ABI-compatible ConfD API (thay thế SDK headers)

deploy/
  k8s.yaml        K8s Deployment + NodePort Service

Dockerfile             SSH server mode (production)
Dockerfile.clitest     Build + test (AlmaLinux 8)
Dockerfile.hostbuild   Build trên Ubuntu (dev)
```

Chi tiết kiến trúc code: xem [ARCHITECTURE.md](ARCHITECTURE.md)
