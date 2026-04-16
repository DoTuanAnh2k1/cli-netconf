# CLI NETCONF — ConfD MAAPI Console

CLI quản lý cấu hình Network Element (NE) qua ConfD MAAPI (Management Agent API).
Kết nối trực tiếp vào ConfD IPC port — không cần NETCONF session hay SSH tới NE.

Hỗ trợ 2 chế độ chạy:
- **SSH Server mode** (mặc định): user SSH vào → login mgt-svc → chọn NE → CLI
- **Direct mode** (debug): kết nối thẳng tới ConfD, bỏ qua login

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

## Chế độ 1: SSH Server Mode (mặc định)

Chế độ chính, dùng khi deploy production. User SSH vào container → CLI tự hỏi
username/password → xác thực qua mgt-svc API → hiển thị danh sách NE để chọn →
kết nối MAAPI tới NE đã chọn.

### Flow

```
User ──SSH──→ [sshd] ──PAM──→ POST /aa/authenticate → mgt-svc
               │                    (xác thực + lấy JWT token)
               │
               ▼ (ForceCommand)
         [cli-netconf]
               │
               │  (nhận token từ PAM, skip login)
               ▼
         GET /aa/list/ne → danh sách NE
               │
         #   Site    NE        ConfD IP     Port
         1   HN01    smf-01    172.19.0.2   23645
         2   HN02    smf-02    172.19.0.3   23645
         Select NE: 1
               │
         MAAPI connect → ConfD NE
               │
         maapi[smf-01]> _
```

Username/password chỉ nhập 1 lần khi SSH. PAM xác thực qua mgt-svc và lưu JWT
token, CLI đọc token đó và nhảy thẳng vào chọn NE.

### Build Docker image

```bash
docker build -f Dockerfile.full -t cli-netconf-full .
```

### Chạy container

```bash
docker run -d -p 2222:22 \
  -v cli-ssh-keys:/etc/ssh/host-keys \
  -e MGT_SVC_BASE=http://mgt-service:8080 \
  -e LOG_LEVEL=info \
  -e SEED_USERNAME=admin \
  -e SEED_PASSWORD=admin123 \
  cli-netconf-full
```

> **Lưu ý**: Mount volume `-v cli-ssh-keys:/etc/ssh/host-keys` để SSH host key
> persist qua restart. Nếu không mount, mỗi lần restart sẽ tạo key mới → client
> SSH báo lỗi "REMOTE HOST IDENTIFICATION HAS CHANGED".

### SSH vào

```bash
ssh admin@localhost -p 2222
# → PAM xác thực qua mgt-svc (dùng password SSH)
# → CLI nhảy thẳng vào chọn NE (không hỏi login lại)
# → Bắt đầu làm việc
```

### Biến môi trường

| Biến | Mặc định | Mô tả |
|---|---|---|
| `MGT_SVC_BASE` | `http://mgt-service:8080` | URL gốc của mgt-svc (authenticate + list NE) |
| `LOG_LEVEL` | `info` | Level log: `debug`, `info`, `warn`, `error`, `off` |
| `SEED_USERNAME` | `anhdt195` | User để sync danh sách user từ mgt-svc |
| `SEED_PASSWORD` | `123` | Password của seed user |

### Deploy lên K8s

```bash
kubectl apply -f deploy/k8s.yaml

# Lấy NodePort
kubectl get svc cli-netconf

# SSH vào
ssh admin@<node-ip> -p <nodeport>
```

---

## Chế độ 2: Direct Mode (debug)

Kết nối thẳng tới ConfD qua MAAPI IPC, bỏ qua login mgt-svc.
Kích hoạt bằng cách set biến `CONFD_IPC_ADDR` hoặc `CONFD_IPC_PORT`.

```bash
CONFD_IPC_ADDR=127.0.0.1 \
CONFD_IPC_PORT=23645 \
NE_NAME=smf \
./cli-netconf
```

### Test nhanh với container ConfD có sẵn

```bash
# Chạy ConfD container
cd confd && docker compose up -d

# Chạy CLI direct mode
docker run --rm -it --network host \
  -e CONFD_IPC_ADDR=127.0.0.1 \
  -e CONFD_IPC_PORT=23645 \
  cli-netconf:clitest ./cli-netconf
```

### Biến môi trường

| Biến | Mặc định | Mô tả |
|---|---|---|
| `CONFD_IPC_ADDR` | `127.0.0.1` | Địa chỉ ConfD IPC |
| `CONFD_IPC_PORT` | `4565` | Port ConfD IPC |
| `MAAPI_USER` | `admin` | Username cho MAAPI session |
| `NE_NAME` | `confd` | Tên hiển thị trong prompt |
| `LOG_LEVEL` | `info` | Level log |
| `LOG_FILE` | `/tmp/cli-netconf.log` | Đường dẫn file log |

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

login <user> [<password>]          Đăng nhập mgt-svc (switch NE trong runtime)
logout                             Xoá token khỏi bộ nhớ
nodes                              List NEs từ mgt-svc

save [--scope=X] <cmd_name>       POST CLI history lên mgt-svc

help                               Danh sách lệnh
exit                               Thoát
```

---

## Path syntax

### Space-separated (khuyến nghị)

```
show running-config eir
show running-config eir nsmfPduSession
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
- `show running-config <TAB>` → config containers (aaa, eir, nacm, ...)
- `show running-config eir <TAB>` → children (nsmfPduSession, networkConfig, ...)
- `set eir nsmfPdu<TAB>` → `nsmfPduSession`
- `dump <TAB>` → `text`, `xml`
- `lock <TAB>` → `running`, `candidate`

Schema tự động load từ ConfD cs_node tree — không cần YANG files.
Chỉ hiển thị config containers (bỏ NETCONF operations, notifications, monitoring).

---

## Logging

Log ghi ra file, không lẫn với stdout/stderr của CLI. Cấu hình qua env:

| Biến | Mặc định | Mô tả |
|---|---|---|
| `LOG_LEVEL` | `info` | `debug` / `info` / `warn` / `error` / `off` |
| `LOG_FILE` | `/tmp/cli-netconf.log` | Đường dẫn file log |

Format:
```
2026-04-16 02:30:48 [INFO ] main.c:1933  cli-netconf started, mode=ssh-server
2026-04-16 02:30:48 [INFO ] main.c:2045  login success: user=admin
2026-04-16 02:30:49 [INFO ] main.c:1602  connected to NE=smf-01 ns=v5gc (172.19.0.2:23645)
2026-04-16 02:31:05 [DEBUG] main.c:1883  cmd: user=admin ne=smf-01 >> show running-config eir
```

Trong SSH server mode (Dockerfile.full), log mỗi user ghi ra file riêng:
`/var/log/cli-netconf/<username>.log`

Khi `LOG_LEVEL=off`: không mở file, zero overhead.

---

## Ví dụ phiên làm việc

### SSH Server mode

```
$ ssh admin@cli-server -p 2222
admin@cli-server's password: ****

=====================================================
   CLI - NETCONF Console
=====================================================

Logged in as admin

Fetching NE list...

#     Site              NE                    Namespace             ConfD IP         Port    Description
────  ────────────────  ────────────────────  ────────────────────  ───────────────  ──────  ──────────────────
1     HN01              smf-01                v5gc                  172.19.0.2       23645   SMF Node 01
2     HN02              smf-02                v5gc                  172.19.0.3       23645   SMF Node 02

Select NE (# or namespace): 1

Connecting to smf-01 (172.19.0.2:23645)...
Connected.
Loading schema...
Schema loaded.
Connected to smf-01 (172.19.0.2:23645)

maapi[smf-01]> show running-config eir
nsmfPduSession true
  is-enabled               true
  status                   active
(10ms)

maapi[smf-01]> set eir nsmfPduSession is-enabled false
OK

maapi[smf-01]> commit
Commit successful. (45ms)

maapi[smf-01]> exit
Goodbye.
```

### Direct mode (debug)

```
$ CONFD_IPC_ADDR=127.0.0.1 CONFD_IPC_PORT=23645 ./cli-netconf

=====================================================
   CLI - NETCONF Console (Direct Debug Mode)
=====================================================

Connecting to ConfD MAAPI 127.0.0.1:23645 ...
Connected.
Loading schema from MAAPI...
Schema loaded.

maapi[confd]> show running-config system
...
```

---

## Paste XML

```
maapi[smf-01]> set
Paste XML config (empty line to finish):
<eir xmlns="http://yang.vht.vn/v5gc/eir/2.0">
  <nsmfPduSession>
    <is-enabled>true</is-enabled>
  </nsmfPduSession>
</eir>

OK (staged in candidate)
maapi[smf-01]> commit
Commit successful.
```

---

## Cấu trúc source

```
src/
  main.c          Entry point, login flow, command loop, tab completion, pager
  maapi-ops.c     MAAPI operations (get/set/commit/lock/stream config)
  maapi.c         MAAPI schema loader (walk ConfD cs_node tree)
  schema.c        Schema tree (build, lookup, YANG parser fallback)
  formatter.c     XML ↔ text converter, extract data

include/
  cli.h           Kiểu dữ liệu chung, hằng số, forward declarations
  log.h           Macro-based logging (zero-cost khi tắt)
  maapi-direct.h  MAAPI session type và function declarations
  confd_compat.h  Tương thích ABI với libconfd.so (thay thế ConfD SDK headers)

confd/                 Container ConfD mẫu (dùng test direct mode)
  docker-compose.yaml  Docker compose cho ConfD
  image/               Dockerfile + config cho ConfD container

deploy/
  k8s.yaml        K8s Deployment + Service cho SSH server mode

Dockerfile             Build CLI binary (direct mode)
Dockerfile.full        SSH server + mgt-svc auth (production)
Dockerfile.clitest     Build + test (AlmaLinux 8, có đủ lib)
Dockerfile.hostbuild   Build nhanh trên Ubuntu (dev)
```

---

## Kiến trúc

```
┌──────────────────────────────────────────────────────────────┐
│ SSH Server Mode (Production — Docker/K8s)                    │
│                                                              │
│  User ──SSH──→ [sshd] ──PAM──→ [mgt-svc /aa/authenticate]   │
│                  │                (xác thực, lưu JWT token)  │
│                  │                                           │
│                  ▼ ForceCommand                              │
│            [cli-netconf]                                     │
│                  │ (đọc token từ PAM, skip login)            │
│                  ▼                                           │
│         GET /aa/list/ne → chọn NE                            │
│                  │                                           │
│                  ▼                                           │
│         [ConfD MAAPI IPC] ──────→ [ConfD NE Pod]             │
└──────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────┐
│ Direct Mode (Debug)                                          │
│                                                              │
│  Terminal ──→ [cli-netconf] ──MAAPI──→ [ConfD]               │
│               (CONFD_IPC_ADDR/PORT)     (IPC port)           │
└──────────────────────────────────────────────────────────────┘
```

---

## Đặc điểm kỹ thuật

- **MAAPI trực tiếp**: không qua NETCONF, kết nối IPC nội bộ ConfD — nhanh, không treo
- **Schema từ ConfD**: `confd_load_schemas` + walk `cs_node` tree — chính xác 100%, không cần YANG files
- **Login + chọn NE**: xác thực qua mgt-svc REST API, hiển thị danh sách NE có quyền, chọn lại nếu connect thất bại
- **Filter namespace**: chỉ hiện config containers, bỏ NETCONF ops/notifications/monitoring
- **Stream config**: dùng `confd_stream_connect` để đọc config qua socket — hoạt động cross-container
- **Show defaults**: flag `MAAPI_CONFIG_SHOW_DEFAULTS` buộc ConfD trả tất cả values kể cả defaults
- **Pager thông minh**: detect terminal height thật (`ioctl TIOCGWINSZ`), `<MORE>` luôn ở dòng cuối
- **Tab completion**: walk schema tree theo path đang gõ, drill down multi-level
- **Logging**: macro-based, zero-cost khi tắt, ghi ra file riêng cho mỗi user
- **ABI tương thích**: `confd_compat.h` map đúng struct layout + versioned symbols của libconfd.so
