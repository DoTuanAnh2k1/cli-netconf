# CLI NETCONF — MAAPI Console

CLI quản lý cấu hình Network Element (NE) qua ConfD MAAPI (Management Agent API).  
Kết nối trực tiếp vào ConfD IPC port — không cần NETCONF session hay SSH.

---

## Yêu cầu

| Thư viện | macOS | Ubuntu/Debian |
|---|---|---|
| libxml2 | `brew install libxml2` | `apt install libxml2-dev` |
| readline | `brew install readline` | `apt install libreadline-dev` |
| libconfd | ConfD SDK (`$CONFD_DIR/lib/libconfd.a`) | — |

ConfD phải đang chạy và có thể kết nối từ máy build/chạy CLI.

---

## Build

```bash
make CONFD_DIR=/path/to/confd
```

Kết quả: file nhị phân `cli-netconf` trong thư mục gốc.

```bash
make clean          # Xoá build artifacts
```

---

## Chạy

```bash
# Kết nối mặc định (127.0.0.1:4565)
./cli-netconf

# Kết nối tới ConfD khác
CONFD_IPC_ADDR=10.0.0.1 CONFD_IPC_PORT=4565 ./cli-netconf
```

### Biến môi trường

| Biến | Mặc định | Mô tả |
|---|---|---|
| `CONFD_IPC_ADDR` | `127.0.0.1` | ConfD host |
| `CONFD_IPC_PORT` | `4565` | ConfD IPC port |
| `MAAPI_USER` | `admin` | Username MAAPI session |
| `NE_NAME` | `confd` | Label hiển thị trong prompt |

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

dump text [file]                   Export config dạng text (tới stdout hoặc file)
dump xml  [file]                   Export config dạng XML

help                               Hiển thị danh sách lệnh
exit                               Thoát
```

---

## Path syntax

CLI hỗ trợ hai cú pháp cho path:

### Space-separated (khuyến nghị)

Dùng dấu cách thay cho `/` và `{}`. Schema tree tự động xác định node nào là list.

```
set system hostname new-host
set system ntp enabled true
set system ntp server 10.0.0.1 prefer true
unset system ntp server 10.0.0.1
show running-config system ntp
```

Với list node, token tiếp theo sau tên list là giá trị key:
```
set interface eth0 description "uplink"
# → /interface{eth0}/description = "uplink"
```

### ConfD keypath (legacy)

Keypath ConfD cũng được chấp nhận khi bắt đầu bằng `/`:

```
set /system/hostname new-host
set /system/ntp/server{10.0.0.1}/prefer true
unset /system/ntp/server{10.0.0.1}
```

---

## Ví dụ phiên làm việc

```
maapi[confd]> show running-config system
system
  hostname  router-1
  ntp
    enabled  true
    server  10.0.0.1
      prefer  true
(12ms)

maapi[confd]> set system hostname router-2
OK

maapi[confd]> set system ntp server 10.0.0.2 prefer false
OK

maapi[confd]> validate
Validation OK.

maapi[confd]> commit
Commit successful. (45ms)

maapi[confd]> dump xml /tmp/backup.xml
Saved to /tmp/backup.xml

maapi[confd]> exit
Goodbye.
```

---

## Paste XML

Dùng `set` không có argument để paste XML config block:

```
maapi[confd]> set
Paste XML config (empty line to finish):
<system>
  <hostname>router-3</hostname>
</system>

OK (staged in candidate)
maapi[confd]> commit
Commit successful. (30ms)
```

---

## Tab completion

- **Lệnh**: Tab ở đầu dòng gợi ý danh sách lệnh.
- **Schema path**: Sau `show`, `set`, `unset` — Tab gợi ý top-level schema nodes.
- Nhấn Tab hai lần để xem tất cả lựa chọn khi có nhiều khả năng.

---

## Docker

```bash
# Build image (cần CONFD_DIR accessible trong build context)
docker build --build-arg CONFD_DIR=/opt/confd -t cli-netconf .

# Chạy container
docker run -it --rm \
  -e CONFD_IPC_ADDR=10.0.0.1 \
  -e CONFD_IPC_PORT=4565 \
  cli-netconf
```

---

## Cấu trúc source

```
src/
  main.c          Entry point, command loop, tab completion
  maapi-ops.c     MAAPI config operations (get/set/commit/lock/...)
  maapi.c         MAAPI schema loader (walk cs_node tree)
  schema.c        Schema tree (build, lookup, YANG parser)
  formatter.c     XML ↔ text converter, pager

include/
  cli.h           Shared types, constants, helpers
  maapi-direct.h  maapi_session_t và function declarations
```
