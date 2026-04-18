# =============================================================================
# Dockerfile — CLI NETCONF C rewrite (SSH Server Mode)
#
# Dùng base image `hsdfat/cli-netconf:ubuntu` (đã có sẵn gcc/make/openssh/…).
# Không gọi apt — dùng được trong môi trường private offline.
#
# Build base (chỉ làm một lần, cần internet):
#   docker build -f Dockerfile.base -t hsdfat/cli-netconf:ubuntu .
#   docker push   hsdfat/cli-netconf:ubuntu
#
# Build app (offline OK, pull base từ registry):
#   docker build -t cli-netconf:latest .
#
# Runtime env vars:
#   MGT_SVC_BASE     — URL gốc mgt-service        (default: http://mgt-service:3000)
#   LOG_LEVEL        — Log level                   (default: info)
#   LOG_STDERR       — 1 = log ra terminal user    (default: 1)
#   SSH_PORT         — Port sshd lắng nghe         (default: 22)
#   SEED_USERNAME    — User để sync users          (default: anhdt195)
#   SEED_PASSWORD    — Password seed user          (default: 123)
#
# Truy cập:
#   ssh <username>@<host> -p <SSH_PORT>
# =============================================================================

FROM hsdfat/cli-netconf:ubuntu

# ── Build binary ─────────────────────────────────────────────────────────────
WORKDIR /build

COPY src/     src/
COPY include/ include/
COPY Makefile .
COPY libconfd-server.so  libconfd.so
COPY libcrypto-server.so libcrypto.so

RUN make CONFD_LIB=/build/libconfd.so \
         LDFLAGS="-lreadline -lxml2 -L/build -lconfd -lcrypto -Wl,-rpath,/usr/lib/cli" \
    && install -m 755 cli-netconf /usr/local/bin/cli-netconf \
    && install -m 755 libconfd.so  /usr/lib/cli/libconfd.so \
    && install -m 755 libcrypto.so /usr/lib/cli/libcrypto.so.1.0.0 \
    && ln -sf /usr/lib/cli/libcrypto.so.1.0.0 /usr/lib/cli/libcrypto.so.10 \
    && echo "/usr/lib/cli" > /etc/ld.so.conf.d/cli.conf \
    && ldconfig \
    && rm -rf /build

WORKDIR /

# ── PAM auth script — xác thực user qua mgt-service API ─────────────────────
COPY <<'AUTH_SCRIPT' /usr/local/bin/auth-mgt.sh
#!/bin/bash
# Gọi POST /aa/authenticate lên mgt-service.
# Input: PAM_USER (env), password (stdin từ pam_exec expose_authtok).
# Output: exit 0 = auth OK, exit 1 = auth fail.
# Log ra /var/log/cli-netconf/auth.log (ghi rõ lý do fail — curl / http / jq).

LOG=/var/log/cli-netconf/auth.log
mkdir -p /var/log/cli-netconf 2>/dev/null
touch "$LOG" 2>/dev/null
chmod 666 "$LOG" 2>/dev/null

log() {
    printf '[%s] [auth] %s\n' "$(date '+%F %T')" "$*" >> "$LOG"
}

# Nạp env từ file duy nhất; sshd child không có env container
[ -f /etc/cli-netconf/env ] && . /etc/cli-netconf/env 2>/dev/null
MGT_URL="${MGT_SVC_BASE:-http://mgt-service:3000}"
read -r PASS

log "attempt user=$PAM_USER rhost=${PAM_RHOST:-?} mgt=$MGT_URL"

if [ -z "$PASS" ]; then
    log "fail user=$PAM_USER reason=empty_password"
    exit 1
fi

ERRFILE=$(mktemp)
RESP=$(curl -s --max-time 5 -w '\nHTTP_CODE:%{http_code}' \
    -X POST "$MGT_URL/aa/authenticate" \
    -H "Content-Type: application/json" \
    -d "{\"username\":\"$PAM_USER\",\"password\":\"$PASS\"}" 2>"$ERRFILE")
CURL_EXIT=$?
CURL_ERR=$(cat "$ERRFILE"); rm -f "$ERRFILE"

if [ $CURL_EXIT -ne 0 ]; then
    log "fail user=$PAM_USER reason=curl_error exit=$CURL_EXIT stderr=${CURL_ERR//$'\n'/ }"
    exit 1
fi

HTTP_CODE=$(printf '%s' "$RESP" | awk -F: '/^HTTP_CODE:/{print $2}')
BODY=$(printf '%s' "$RESP" | sed '$d')

if [ "$HTTP_CODE" != "200" ]; then
    log "fail user=$PAM_USER reason=http_status code=$HTTP_CODE body=${BODY//$'\n'/ }"
    exit 1
fi

STATUS=$(printf '%s' "$BODY" | jq -r '.status // empty' 2>/dev/null)
TOKEN=$(printf '%s'  "$BODY" | jq -r '.response_data // empty' 2>/dev/null)

if [ "$STATUS" = "success" ] && [ -n "$TOKEN" ]; then
    mkdir -p /tmp/cli-tokens
    printf '%s' "$TOKEN" > "/tmp/cli-tokens/$PAM_USER"
    log "success user=$PAM_USER token_len=${#TOKEN}"
    exit 0
fi

log "fail user=$PAM_USER reason=bad_response status='$STATUS' token_len=${#TOKEN} body=${BODY//$'\n'/ }"
exit 1
AUTH_SCRIPT

RUN chmod +x /usr/local/bin/auth-mgt.sh

# ── CLI wrapper — SSH Server Mode (không set CONFD env → CLI tự login + chọn NE)
COPY <<'CLI_WRAPPER' /usr/local/bin/cli-wrapper.sh
#!/bin/bash
# Wrapper tối giản — KHÔNG set config env.
# Config (MGT_SVC_BASE, LOG_LEVEL, …) do cli-netconf tự đọc từ /etc/cli-netconf/env.
# Wrapper chỉ lo:
#   1) LD_LIBRARY_PATH (phải set trước exec, dynamic linker cần)
#   2) MAAPI_USER / token / session log
#   3) Exec cli-netconf

export LD_LIBRARY_PATH=/usr/lib/cli
export MAAPI_USER="${USER:-admin}"

SESSION_LOG=/var/log/cli-netconf/session.log
mkdir -p /var/log/cli-netconf
touch "$SESSION_LOG" 2>/dev/null
chmod 666 "$SESSION_LOG" 2>/dev/null

slog() {
    printf '[%s] [session] %s\n' "$(date '+%F %T')" "$*" | tee -a "$SESSION_LOG" >&2
}

# PAM auth đã lưu JWT token → truyền cho CLI để skip login prompt
TOKEN_FILE="/tmp/cli-tokens/$USER"
if [ -f "$TOKEN_FILE" ]; then
    export MGT_SVC_TOKEN="$(cat "$TOKEN_FILE")"
    export MGT_SVC_USER="$USER"
    slog "start user=$USER rhost=${SSH_CLIENT%% *} token=yes"
else
    slog "start user=$USER rhost=${SSH_CLIENT%% *} token=NO (missing: $TOKEN_FILE)"
fi

trap 'slog "end user=$USER exit=$?"' EXIT

exec /usr/local/bin/cli-netconf
CLI_WRAPPER

RUN chmod +x /usr/local/bin/cli-wrapper.sh

# ── Cấu hình SSH server ──────────────────────────────────────────────────────
RUN sed -i \
        -e 's/#PasswordAuthentication yes/PasswordAuthentication yes/' \
        -e 's/#PermitRootLogin.*/PermitRootLogin no/' \
        -e 's/#UsePAM.*/UsePAM yes/' \
        /etc/ssh/sshd_config \
    && echo "ForceCommand /usr/local/bin/cli-wrapper.sh" >> /etc/ssh/sshd_config \
    && echo "PrintMotd no"      >> /etc/ssh/sshd_config \
    && echo "PrintLastLog no"   >> /etc/ssh/sshd_config \
    && echo "AllowAgentForwarding no" >> /etc/ssh/sshd_config \
    && echo "AllowTcpForwarding no"   >> /etc/ssh/sshd_config

# ── PAM: xác thực qua script thay vì /etc/shadow ────────────────────────────
COPY <<'PAM_CFG' /etc/pam.d/sshd
auth    required  pam_exec.so  expose_authtok  /usr/local/bin/auth-mgt.sh
auth    optional  pam_permit.so
account required  pam_permit.so
session required  pam_limits.so
session required  pam_permit.so
PAM_CFG

# PAM cần user tồn tại trong system để tạo session
# Tạo một system user dùng chung; ForceCommand quyết định gì chạy
RUN useradd -m -s /bin/bash -G sudo cliuser \
    && echo "cliuser:!" | chpasswd

# ── Sync users script — fetch từ mgt-service, tạo system users cho SSH ───────
COPY <<'SYNC_SCRIPT' /usr/local/bin/sync-users.sh
#!/bin/bash
# sshd cần user tồn tại trong /etc/passwd trước khi PAM auth chạy.
# Script này lấy danh sách user từ mgt-service và tạo system users.

MGT_URL="${MGT_SVC_BASE:-http://mgt-service:3000}"
SEED_USER="${SEED_USERNAME:-anhdt195}"
SEED_PASS="${SEED_PASSWORD:-123}"

TOKEN=$(curl -sf --max-time 10 -X POST "$MGT_URL/aa/authenticate" \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$SEED_USER\",\"password\":\"$SEED_PASS\"}" 2>/dev/null \
  | jq -r '.response_data // empty' 2>/dev/null)

if [ -z "$TOKEN" ]; then
  echo "sync-users: cannot login to mgt-service, creating seed user only"
  id "$SEED_USER" &>/dev/null || useradd -m -s /bin/bash "$SEED_USER" 2>/dev/null
  exit 0
fi

USERS=$(curl -sf --max-time 10 -H "Authorization: $TOKEN" \
  "$MGT_URL/aa/authenticate/user/show" 2>/dev/null)

if [ -n "$USERS" ]; then
  printf '%s' "$USERS" | jq -r '
    if type == "array" then .[].username
    else empty end
  ' 2>/dev/null | while read -r USERNAME; do
    if [ -n "$USERNAME" ] && ! id "$USERNAME" &>/dev/null; then
      useradd -m -s /bin/bash "$USERNAME" 2>/dev/null
      echo "  sync-users: created $USERNAME"
    fi
  done
  echo "sync-users: done"
else
  echo "sync-users: cannot fetch user list, creating seed user only"
  id "$SEED_USER" &>/dev/null || useradd -m -s /bin/bash "$SEED_USER" 2>/dev/null
fi
SYNC_SCRIPT

RUN chmod +x /usr/local/bin/sync-users.sh

# ── Entrypoint ───────────────────────────────────────────────────────────────
COPY <<'ENTRYPOINT' /entrypoint.sh
#!/bin/bash
ssh-keygen -A 2>/dev/null

# Ghi env vào /etc/cli-netconf/env — nguồn config duy nhất cho mọi child
# process của sshd (wrapper, auth script, và chính binary cli-netconf).
# Binary gọi load_env_file() ở startup để đọc file này.
mkdir -p /etc/cli-netconf
SSH_PORT="${SSH_PORT:-22}"

cat > /etc/cli-netconf/env <<EOF
MGT_SVC_BASE=${MGT_SVC_BASE:-http://mgt-service:3000}
LOG_LEVEL=${LOG_LEVEL:-info}
LOG_STDERR=${LOG_STDERR:-1}
EOF
chmod 644 /etc/cli-netconf/env

mkdir -p /var/log/cli-netconf
chmod 1777 /var/log/cli-netconf

echo "=== CLI NETCONF SSH Server Mode ==="
echo "  MGT_SVC_BASE:  ${MGT_SVC_BASE:-http://mgt-service:3000}"
echo "  LOG_LEVEL:     ${LOG_LEVEL:-info}"
echo "  LOG_STDERR:    ${LOG_STDERR:-1} (1 = log ra terminal user)"
echo "  SSH port:      ${SSH_PORT}"
echo "  Config file:   /etc/cli-netconf/env"
echo "  Logs:"
echo "    /var/log/cli-netconf/auth.log     (PAM auth vs mgt-service)"
echo "    /var/log/cli-netconf/session.log  (SSH session start/end)"
echo "    /var/log/cli-netconf/<user>.log   (per-user cli-netconf log)"
echo ""

# Sync users từ mgt-service
/usr/local/bin/sync-users.sh

# Tail log files ra stdout container để docker logs xem được real-time
touch /var/log/cli-netconf/auth.log /var/log/cli-netconf/session.log
chmod 666 /var/log/cli-netconf/auth.log /var/log/cli-netconf/session.log
tail -F /var/log/cli-netconf/auth.log /var/log/cli-netconf/session.log &

echo ""
echo "  SSH listening on port ${SSH_PORT}"
echo ""

exec /usr/sbin/sshd -D -e -p "${SSH_PORT}"
ENTRYPOINT

RUN chmod +x /entrypoint.sh

EXPOSE 22

ENV MGT_SVC_BASE=http://mgt-service:3000 \
    LOG_LEVEL=info \
    LOG_STDERR=1 \
    SSH_PORT=22 \
    SEED_USERNAME=anhdt195 \
    SEED_PASSWORD=123

ENTRYPOINT ["/entrypoint.sh"]
