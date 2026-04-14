# CLI NETCONF — ConfD MAAPI Console

CLI quản lý cấu hình Network Element (NE) qua ConfD MAAPI (Management Agent API).
Kết nối trực tiếp vào ConfD IPC port — không cần NETCONF session hay SSH tới NE.

Hỗ trợ 2 chế độ chạy:
- **Direct mode**: chạy binary trực tiếp, kết nối thẳng tới ConfD
- **Full mode**: SSH server trong Docker/K8s, xác thực qua mgt-service API

---

## Yêu cầu build

| Thư viện | Ubuntu/Debian | macOS |
|---|---|---|
| gcc | `apt install gcc` | Xcode CLI tools |
| libxml2 | `apt install libxml2-dev` | `brew install libxml2` |
| readline | `apt install libreadline-dev` | `brew install readline` |
| libconfd.so | Từ ConfD container hoặc SDK | — |

---

## Build

```bash
# Lấy libconfd.so từ container ConfD đang chạy
docker cp <confd-container>:/usr/lib64/libconfd.so ./libconfd-server.so
docker cp <confd-container>:/usr/lib64/libcrypto.so.10 ./libcrypto-server.so

# Build
make CONFD_LIB=./libconfd-server.so \
     LDFLAGS="-lreadline -lxml2 -L. -lconfd-server -Wl,-rpath,."

# Dọn
make clean
```

---

## Chế độ 1: Direct Mode

Chạy binary trực tiếp trên máy, kết nối tới ConfD qua MAAPI IPC.

```bash
LD_LIBRARY_PATH=. \
CONFD_IPC_ADDR=172.19.0.2 \
CONFD_IPC_PORT=23645 \
NE_NAME=smf \
./cli-netconf
```

### Biến môi trường

| Biến | Mặc định | Mô tả |
|---|---|---|
| `CONFD_IPC_ADDR` | `127.0.0.1` | Địa chỉ ConfD IPC |
| `CONFD_IPC_PORT` | `4565` | Port ConfD IPC |
| `MAAPI_USER` | `admin` | Username cho MAAPI session |
| `NE_NAME` | `confd` | Tên hiển thị trong prompt |

---

## Chế độ 2: Full Mode (SSH + mgt-service)

Chạy trong Docker/K8s. User SSH vào → xác thực qua mgt-service REST API → tự động chạy CLI.

### Build Docker image

```bash
docker build -f Dockerfile.full -t cli-netconf-full .
```

### Chạy container

```bash
docker run -d -p 2222:22 \
  -e CONFD_IPC_ADDR=172.19.0.2 \
  -e CONFD_IPC_PORT=23645 \
  -e MGT_SERVICE_URL=http://mgt-service:3000 \
  -e NE_NAME=smf \
  cli-netconf-full
```

### SSH vào

```bash
ssh admin@localhost -p 2222
# Mật khẩu xác thực qua mgt-service API
```

### Biến môi trường (Full mode)

| Biến | Mặc định | Mô tả |
|---|---|---|
| `CONFD_IPC_ADDR` | `127.0.0.1` | Địa chỉ ConfD IPC |
| `CONFD_IPC_PORT` | `4565` | Port ConfD IPC |
| `MGT_SERVICE_URL` | `http://mgt-service:3000` | URL mgt-service để xác thực |
| `NE_NAME` | `confd` | Tên NE hiển thị trong prompt |

### Deploy lên K8s

```bash
kubectl apply -f deploy/k8s.yaml

# Lấy NodePort
kubectl get svc cli-netconf

# SSH vào
ssh admin@<node-ip> -p <nodeport>
```

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

## Ví dụ phiên làm việc

```
maapi[eir]> show running-config eir
nsmfPduSession true
  is-enabled               true
  status                   active
networkConfig tcp
  protocol                 tcp
  timeout                  30
  max-retries              3
(10ms)

maapi[eir]> set eir nsmfPduSession is-enabled false
OK

maapi[eir]> show candidate-config eir nsmfPduSession
is-enabled               false
status                   active

maapi[eir]> validate
Validation OK.

maapi[eir]> commit
Commit successful. (45ms)

maapi[eir]> dump xml /tmp/backup.xml
Saved to /tmp/backup.xml

maapi[eir]> exit
Goodbye.
```

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

## Cấu trúc source

```
src/
  main.c          Entry point, vòng lệnh chính, tab completion, pager
  maapi-ops.c     MAAPI operations (get/set/commit/lock/stream config)
  maapi.c         MAAPI schema loader (walk ConfD cs_node tree)
  schema.c        Schema tree (build, lookup, YANG parser fallback)
  formatter.c     XML ↔ text converter, extract data

include/
  cli.h           Kiểu dữ liệu chung, hằng số, forward declarations
  maapi-direct.h  MAAPI session type và function declarations
  confd_compat.h  Tương thích ABI với libconfd.so (thay thế ConfD SDK headers)

deploy/
  k8s.yaml        K8s Deployment + Service cho full mode

Dockerfile        Build CLI binary (direct mode)
Dockerfile.full   SSH server + mgt-service auth (full mode)
config.yang       YANG model mẫu (tuỳ từng NE)
```

---

## Kiến trúc

```
┌─────────────────────────────────────────────────────────┐
│ Full Mode (Docker/K8s)                                  │
│                                                         │
│  User ──SSH──→ [sshd] ──PAM──→ [mgt-service API]       │
│                  │                   (xác thực)         │
│                  │                                      │
│                  ▼                                      │
│            [cli-netconf]                                │
│                  │                                      │
│                  ▼                                      │
│         [ConfD MAAPI IPC] ──────→ [ConfD NE Pod]        │
│                                   (running config)      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ Direct Mode                                             │
│                                                         │
│  Terminal ──→ [cli-netconf] ──MAAPI──→ [ConfD]          │
│                                        (IPC port)       │
└─────────────────────────────────────────────────────────┘
```

---

## Đặc điểm kỹ thuật

- **MAAPI trực tiếp**: không qua NETCONF, kết nối IPC nội bộ ConfD — nhanh, không treo
- **Schema từ ConfD**: `confd_load_schemas` + walk `cs_node` tree — chính xác 100%, không cần YANG files
- **Filter namespace**: chỉ hiện config containers, bỏ NETCONF ops/notifications/monitoring
- **Stream config**: dùng `confd_stream_connect` để đọc config qua socket — hoạt động cross-container
- **Show defaults**: flag `MAAPI_CONFIG_SHOW_DEFAULTS` buộc ConfD trả tất cả values kể cả defaults
- **Pager thông minh**: detect terminal height thật (`ioctl TIOCGWINSZ`), `<MORE>` luôn ở dòng cuối
- **Tab completion**: walk schema tree theo path đang gõ, drill down multi-level
- **ABI tương thích**: `confd_compat.h` map đúng struct layout + versioned symbols của libconfd.so
