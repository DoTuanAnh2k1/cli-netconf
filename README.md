# CLI NETCONF — ConfD MAAPI Console

CLI quản lý cấu hình Network Element (NE) qua ConfD MAAPI (Management Agent API).
Kết nối trực tiếp vào ConfD IPC port — không cần NETCONF session hay SSH tới NE.

Hỗ trợ 3 chế độ chạy:
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

# Build binary
make CONFD_LIB=./libconfd-server.so

# Dọn
make clean
```

### Build Docker image (2 tầng: base + app)

Base image `hsdfat/cli-netconf:ubuntu` chứa sẵn apt deps (gcc, openssh, …)
— build 1 lần, push lên Docker Hub. Dockerfile chính `FROM` base đó, không
gọi `apt` nên build được trong môi trường private offline.

```bash
# Base (chỉ khi cần update deps)
docker build -f Dockerfile.base -t hsdfat/cli-netconf:ubuntu .
docker push  hsdfat/cli-netconf:ubuntu

# App (offline OK — pull base từ registry)
docker build -t hsdfat/cli-netconf:latest .
docker push  hsdfat/cli-netconf:latest

# Pull ảnh đã build sẵn
docker pull hsdfat/cli-netconf:latest
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
         #   NE        ConfD IP     Port
         1   smf-01    172.19.0.2   23645
         2   smf-02    172.19.0.3   23645
         Select NE: 1
               │
         MAAPI connect → conf_master_ip:conf_port_master_tcp
               │
         anhdt195[smf-01]> _
```

### Build & chạy Docker

```bash
# Build
docker build -t cli-netconf .

# Chạy
docker run -d \
  -e SSH_PORT=2222 \
  -p 2222:2222 \
  -v cli-ssh-keys:/etc/ssh/host-keys \
  -e MGT_SVC_BASE=http://mgt-service:8080 \
  -e LOG_LEVEL=info \
  -e LOG_STDERR=1 \
  -e SEED_USERNAME=admin \
  -e SEED_PASSWORD=admin123 \
  cli-netconf

# SSH vào
ssh admin@localhost -p 2222
```

> **Lưu ý**:
> - Mount volume `-v cli-ssh-keys:/etc/ssh/host-keys` để SSH host key persist qua
>   restart, tránh lỗi "REMOTE HOST IDENTIFICATION HAS CHANGED".
> - Port container (`-p X:Y`) phải khớp với `SSH_PORT=Y` đã set.

### Biến môi trường

Set qua `docker run -e ...` / docker-compose. Entrypoint ghi vào
`/etc/cli-netconf/env`, binary `cli-netconf` tự đọc file này khi khởi động
(không phụ thuộc env shell — sshd child không kế thừa env container).

| Biến | Mặc định | Mô tả |
|---|---|---|
| `MGT_SVC_BASE` | `http://mgt-service:3000` | URL gốc mgt-svc (authenticate + list NE) |
| `SSH_PORT` | `22` | Port sshd lắng nghe bên trong container |
| `LOG_LEVEL` | `info` | Level log: `debug` / `info` / `warn` / `error` / `off` |
| `LOG_STDERR` | `1` | `1` = log hiện trên terminal user khi SSH vào, `0` = chỉ ghi file |
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

anhdt195[confd]> login anhdt195
Password: ****
Logged in as anhdt195

Fetching NE list...

#     NE     Namespace   ConfD IP       Port    Description
────  ─────  ──────────  ─────────────  ──────  ──────────────────
1     eir    hteir01     172.19.0.2     23645   EIR - Equipment Identity Register

Select NE (# or namespace): 1

Connecting to eir (172.19.0.2:23645)...
Loading schema...
Schema loaded.
Connected to eir (172.19.0.2:23645)

anhdt195[eir]> show running-config
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
| `NE_IP` | — | IP NE (override, mặc định lấy từ MAAPI host) |
| `LOG_LEVEL` | `info` | Level log |
| `LOG_FILE` | `/tmp/cli-netconf.log` | Đường dẫn file log |
| `LOG_STDERR` | auto | `1` = log ra stderr, auto-on nếu stderr là TTY |

---

## Lệnh

```
show running-config [path...]      Xem cấu hình running
show candidate-config [path...]    Xem cấu hình candidate

set <path...> <value>              Đặt giá trị leaf
set <path...> <list-name> <key>    Tạo list entry + batch-set nhiều leaf
    <leaf1> <v1> [<leaf2> <v2>...]
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
                                     (scope mặc định: ne-config;
                                      enum: cli-config | ne-command | ne-config)

help                               Danh sách lệnh
exit                               Trong SSH mode: quay về màn chọn NE.
                                     Direct mode: thoát chương trình.
                                     Ctrl+D thoát hẳn ra SSH.
```

---

## Path syntax

### Space-separated (khuyến nghị)

```
show running-config eir
set eir nsmfPduSession is-enabled true
unset eir networkConfig
```

### Set với YANG list

YANG:
```yang
list subscriber {
  key name;
  leaf name  { type string; }
  leaf role  { type string; }
  leaf email { type string; }
}
```

Cú pháp: `set <container...> <list-name> <key-value> [<leaf> <value> ...]`

- **Chỉ cần gõ key value một lần** — không phải lặp lại tên key (`name`) nữa
  vì ConfD tự lưu key từ `{john}` trong path.
- **Batch**: sau key value, có thể truyền nhiều cặp `<leaf> <value>` để set
  luôn tất cả trong một lệnh.

```
# Đặt 1 leaf
set eir subscriber john role admin

# Đặt nhiều leaf trong một phát (tạo luôn entry với key=john nếu chưa có)
set eir subscriber john role admin email john@x.com
```

Lỗi cú pháp sẽ in ra danh sách các tên con tại vị trí hiện tại, ví dụ:

```
Unknown node 'rol' at path /eir/subscriber{john}
  available: name, role [list], email, groups [list]
```

### ConfD keypath (legacy)

```
set /eir/nsmfPduSession/is-enabled true
set /eir/subscriber{john}/role admin
unset /eir/networkConfig
```

---

## Prompt format

```
<user>[<ne>]>
```

`<user>` lấy từ env `MAAPI_USER` (wrapper SSH set = username đăng nhập),
fallback `USER` → `LOGNAME` → `admin`. `<ne>` là tên NE đang kết nối
(hoặc `confd` khi chưa chọn NE).

Ví dụ: `anhdt195[smf-01]>`

---

## Config output format

`show running-config [path...]` in ra cây config với 3 quy ước:

1. **Dòng đầu = path filter** — các phần tử path cách nhau bởi space.
   Không có filter → không có dòng path.
2. **Các dòng sau = cây config** — mỗi cấp thụt vào 1 ký tự `\t`. Các leaf
   cùng 1 container luôn có số tab bằng nhau.
3. **Leaf = `name: value`** — dấu `:` phân tách tên và giá trị.

```
anhdt195[eir]> show running-config eir nsmfPduSession
eir nsmfPduSession
	is-enabled: true
	max-sessions: 100
	timeout
		idle: 300
		hard: 3600
(12ms, 2026-04-18 14:30:45)
```

Dòng cuối = `(duration, timestamp)` — duration là thời gian thực hiện,
timestamp là thời điểm lệnh kết thúc (cũng áp dụng cho `commit`).

List entry (có sibling cùng tên) in kèm key ngay cạnh tên:

```
anhdt195[eir]> show running-config servers
servers
	server ntp1
		name: ntp1
		address: 10.0.0.1
	server ntp2
		name: ntp2
		address: 10.0.0.2
(8ms, 2026-04-18 14:30:52)
```

---

## Save command history (mgt-svc)

`save <cmd_name>` POST lịch sử lệnh lên mgt-svc qua `POST /aa/history/save`.

```
save [--scope=X] [--result=Y] [--ne=NAME] [--ip=IP] <cmd_name...>
```

Trường `scope` quyết định log vào nhóm nào — mgt-svc filter theo
query `?scope=...` ở `GET /aa/history`.

| scope | Dùng khi | Mặc định |
|---|---|---|
| `ne-config` | Thao tác cấu hình NE (set/unset/commit/rollback…) | ✓ |
| `ne-command` | Lệnh vận hành NE (show/ping/debug…) | |
| `cli-config` | Config của chính CLI, không thuộc NE | |

Ví dụ:

```
anhdt195[eir]> save set-pdu-enabled                 # scope=ne-config (mặc định)
anhdt195[eir]> save --scope=ne-command show-status
anhdt195[eir]> save --result=failure --scope=ne-config set-timeout
```

Token dùng cho header `Authorization` lấy từ session hiện tại (`login` /
SSH-PAM) — không cần set thủ công.

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
anhdt195[eir]> set
Paste XML config (empty line to finish):
<eir xmlns="http://yang.vht.vn/v5gc/eir/2.0">
  <nsmfPduSession>
    <is-enabled>true</is-enabled>
  </nsmfPduSession>
</eir>

OK (staged in candidate)
anhdt195[eir]> commit
Commit successful.
```

---

## Logging

### Env điều khiển

| Biến | Mặc định | Mô tả |
|---|---|---|
| `LOG_LEVEL` | `info` | `debug` / `info` / `warn` / `error` / `off` |
| `LOG_FILE` | per-user trong SSH mode | Path file log (tự tính `/var/log/cli-netconf/<user>.log` nếu có `$USER`) |
| `LOG_STDERR` | `1` (hoặc auto nếu stderr là TTY) | Song song ghi log ra terminal user |
| `LOG_PID1` | `1` trong Docker image | Ghi log vào `/proc/1/fd/2` (stderr của sshd PID 1) để xuất hiện trong `docker logs` |

### Các nguồn log trong SSH Server Mode

| File | Ghi bởi | Nội dung |
|---|---|---|
| `/var/log/cli-netconf/auth.log` | `auth-mgt.sh` (PAM) | Mỗi lần SSH login thử: user, HTTP code, curl error, reason fail |
| `/var/log/cli-netconf/session.log` | wrapper | Start/end session: user, rhost, token có/không |
| `/var/log/cli-netconf/<user>.log` | `cli-netconf` | Lệnh user gõ, kết nối NE, commit, error |
| `docker logs <container>` | auth-mgt.sh + cli-netconf (`LOG_PID1=1`) | Auth fail reason + mọi dòng log INFO/WARN/ERROR của CLI real-time |

Khi `LOG_STDERR=1`, log binary cũng hiện luôn trên terminal user vừa SSH vào —
thấy được ngay `[INFO]` / `[WARN]` / `[ERROR]` mà không phải `docker exec tail`.

Khi `LOG_PID1=1` (bật mặc định trong ảnh Docker), mọi log của binary đều được
copy sang stderr của PID 1 (sshd), nên `docker logs <container>` sẽ thấy đầy
đủ chuỗi thao tác của từng user real-time — không phải `docker exec` vào tail.

### Các log được ghi ở mức INFO

Ngoài những sự kiện về session (login, logout, connect NE), từ tag `18046`
cli-netconf log ở mức INFO cho từng thao tác của user để admin có thể tra cứu
"ai gõ lệnh gì vào NE nào lúc nào":

| Sự kiện | Mẫu log |
|---|---|
| Session bắt đầu | `session start: user=phatlc rhost=10.0.0.5` |
| Fetch NE list | `fetching NE list for user=phatlc` |
| Chọn NE | `NE selected: user=phatlc rhost=10.0.0.5 NE=smf-01 (172.19.0.2:23645)` |
| Kết nối MAAPI | `connected to NE=smf-01 ns=v5gc (...) user=phatlc` |
| Mỗi lệnh user gõ | `cmd: user=phatlc rhost=10.0.0.5 ne=smf-01 >> show running-config eir` |
| `show` kết quả | `show OK: user=phatlc ne=smf-01 ds=running-config (42ms)` |
| `set` / `unset` | `set OK: user=phatlc ne=smf-01 path=/system/hostname value=new-host` |
| `commit` | `commit OK: user=phatlc ne=smf-01 (18ms)` / `commit FAILED: ...` |
| `validate` / `discard` / `lock` / `unlock` | `<cmd> OK: user=phatlc ne=smf-01 ...` |
| `save` → mgt-svc | `save OK: user=phatlc ne=smf-01 scope=ne-config result=success` |
| `exit` về NE select | `exit from NE=smf-01 → back to NE select` |

### Format file log

```
2026-04-18 02:30:48 [INFO ] main.c:2075  cli-netconf started, mode=ssh-server
2026-04-18 02:30:48 [INFO ] main.c:2017  session start: user=phatlc rhost=10.0.0.5
2026-04-18 02:30:49 [INFO ] main.c:1636  fetching NE list for user=phatlc
2026-04-18 02:30:50 [INFO ] main.c:1687  NE selected: user=phatlc rhost=10.0.0.5 NE=smf-01 (172.19.0.2:23645)
2026-04-18 02:30:51 [INFO ] main.c:1710  connected to NE=smf-01 ns=v5gc (172.19.0.2:23645) user=phatlc
2026-04-18 02:31:05 [INFO ] main.c:1961  cmd: user=phatlc rhost=10.0.0.5 ne=smf-01 >> show running-config eir
2026-04-18 02:31:05 [INFO ] main.c:542   show OK: user=phatlc ne=smf-01 ds=running-config (27ms)
2026-04-18 02:31:22 [INFO ] main.c:700   set OK: user=phatlc ne=smf-01 path=/system/hostname value=edge-01
2026-04-18 02:31:25 [INFO ] main.c:763   commit OK: user=phatlc ne=smf-01 (15ms)
```

Trong `docker logs`, format gọn hơn (bỏ file:line):

```
2026-04-18 02:31:05 [cli] [INFO ] cmd: user=phatlc rhost=10.0.0.5 ne=smf-01 >> show running-config eir
2026-04-18 02:31:05 [cli] [INFO ] show OK: user=phatlc ne=smf-01 ds=running-config (27ms)
```

Khi `LOG_LEVEL=off`: không mở file, zero overhead.

### Debug auth thất bại

Khi SSH báo `Permission denied`, xem `auth.log` để biết lý do cụ thể:

```
[2026-04-16 10:12:03] [auth] attempt user=phatlc rhost=10.0.0.5 mgt=http://mgt-service:3000
[2026-04-16 10:12:03] [auth] fail user=phatlc reason=http_status code=401 body={"status":"fail","message":"invalid credentials"}
```

Các reason thường gặp: `curl_error` (mgt-service unreachable), `http_status`
(sai password / user không tồn tại), `bad_response` (response JSON không có
token), `empty_password`.

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
  log.h           Macro-based logging (zero-cost khi tắt) + env file loader
  maapi-direct.h  MAAPI session type, function declarations
  confd_compat.h  ABI-compatible ConfD API (thay thế SDK headers)

deploy/
  k8s.yaml        K8s Deployment + NodePort Service

Dockerfile        SSH server mode (production) — all-in-one
```

Chi tiết kiến trúc code: xem [ARCHITECTURE.md](ARCHITECTURE.md)
