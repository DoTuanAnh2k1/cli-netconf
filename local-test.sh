#!/usr/bin/env bash
# ================================================================
# local-test.sh — Khởi động môi trường test local
#
# Chạy:  bash local-test.sh [mode]
#
#   mode:
#     go-ssh      SSH server Go  → ssh admin@127.0.0.1 -p 2222
#     go-direct   Go direct TCP  (default)
#     c           C full mode    (mgt-service + NETCONF)
#     c-direct    C direct TCP   (không cần mgt-service)
#
# Không có mode → hiện menu chọn.
# ================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
GO_DIR="$ROOT/go"
C_DIR="$ROOT/c"

GREEN='\033[32m'; YELLOW='\033[33m'; CYAN='\033[36m'
BOLD='\033[1m'; DIM='\033[90m'; RESET='\033[0m'

PID_MGT=""; PID_NC=""

# ── Cleanup ────────────────────────────────────────────────────
cleanup() {
    echo -e "\n${YELLOW}Stopping mock services...${RESET}"
    [ -n "$PID_MGT" ] && kill "$PID_MGT" 2>/dev/null && wait "$PID_MGT" 2>/dev/null || true
    [ -n "$PID_NC"  ] && kill "$PID_NC"  2>/dev/null && wait "$PID_NC"  2>/dev/null || true
    echo -e "${GREEN}Done.${RESET}"
}
trap cleanup EXIT

# ── Build helpers ──────────────────────────────────────────────
build_go() {
    echo -e "${CYAN}Building Go binaries...${RESET}"
    cd "$GO_DIR"
    go build -o bin/cli-netconf  ./cmd/netconf  2>&1
    go build -o bin/cli-direct   ./cmd/direct   2>&1
    go build -o bin/mock-mgt     ./test/mock-mgt     2>&1
    go build -o bin/mock-netconf ./test/mock-netconf 2>&1
    echo -e "${GREEN}Go build OK${RESET}"
    cd "$ROOT"
}

build_c() {
    echo -e "${CYAN}Building C binary...${RESET}"
    make -C "$C_DIR" -j4 2>&1
    echo -e "${GREEN}C build OK${RESET}"
}

build_c_direct() {
    echo -e "${CYAN}Building C direct binary...${RESET}"
    make -C "$C_DIR" -j4 direct 2>&1
    echo -e "${GREEN}C direct build OK${RESET}"
}

# ── Start mocks ────────────────────────────────────────────────
start_mock_mgt() {
    echo -e "${CYAN}Starting mock mgt-service on :3000...${RESET}"
    cd "$GO_DIR"
    NETCONF_PORT=8830 ./bin/mock-mgt &
    PID_MGT=$!
    cd "$ROOT"
    sleep 0.4
    echo -e "  ${DIM}PID $PID_MGT${RESET}"
}

start_mock_netconf() {
    echo -e "${CYAN}Starting mock NETCONF server SSH:8830 / TCP:2023...${RESET}"
    cd "$GO_DIR"
    ./bin/mock-netconf &
    PID_NC=$!
    cd "$ROOT"
    sleep 0.4
    echo -e "  ${DIM}PID $PID_NC${RESET}"
}

wait_ctrl_c() {
    echo ""
    echo -e "  Press ${YELLOW}Ctrl+C${RESET} to stop mock services."
    echo ""
    wait
}

# ── Modes ──────────────────────────────────────────────────────

mode_go_ssh() {
    build_go
    start_mock_mgt
    start_mock_netconf

    echo ""
    echo -e "${GREEN}${BOLD}============================================${RESET}"
    echo -e "${GREEN}${BOLD}  Go SSH server mode${RESET}"
    echo -e "${GREEN}${BOLD}============================================${RESET}"
    echo ""
    echo -e "  Mock mgt-service : ${CYAN}http://127.0.0.1:3000${RESET}"
    echo -e "  Mock NETCONF     : ${CYAN}127.0.0.1:8830 (SSH) / :2023 (TCP)${RESET}"
    echo ""
    echo -e "  ${YELLOW}Kết nối CLI:${RESET}"
    echo -e "    ${BOLD}ssh admin@127.0.0.1 -p 2222${RESET}   # password: admin"
    echo ""
    echo -e "  ${YELLOW}Khởi động SSH server (terminal khác):${RESET}"
    echo -e "    cd go && MGT_SERVICE_URL=http://127.0.0.1:3000 ./bin/cli-netconf"
    echo ""

    wait_ctrl_c
}

mode_go_direct() {
    build_go
    start_mock_mgt
    start_mock_netconf

    echo ""
    echo -e "${GREEN}${BOLD}============================================${RESET}"
    echo -e "${GREEN}${BOLD}  Go direct mode (TCP :2023)${RESET}"
    echo -e "${GREEN}${BOLD}============================================${RESET}"
    echo ""
    echo -e "  Mock mgt-service : ${CYAN}http://127.0.0.1:3000${RESET}"
    echo -e "  Mock NETCONF     : ${CYAN}127.0.0.1:2023 (TCP)${RESET}"
    echo ""
    echo -e "  ${YELLOW}Chạy Go direct:${RESET}"
    echo ""

    cd "$GO_DIR"
    MGT_URL=http://127.0.0.1:3000 \
    NETCONF_HOST=127.0.0.1 \
    NETCONF_PORT=2023 \
    NETCONF_MODE=tcp \
    NE_NAME=mock-ne \
    ./bin/cli-direct
    cd "$ROOT"
}

mode_c() {
    build_go     # cần mock servers
    build_c
    start_mock_mgt
    start_mock_netconf

    echo ""
    echo -e "${GREEN}${BOLD}============================================${RESET}"
    echo -e "${GREEN}${BOLD}  C full mode (mgt-service + NETCONF TCP)${RESET}"
    echo -e "${GREEN}${BOLD}============================================${RESET}"
    echo ""
    echo -e "  Mock mgt-service : ${CYAN}http://127.0.0.1:3000${RESET}"
    echo -e "  Mock NETCONF     : ${CYAN}127.0.0.1:2023 (TCP)${RESET}"
    echo ""

    MGT_URL=http://127.0.0.1:3000 \
    NETCONF_HOST=127.0.0.1 \
    NETCONF_PORT=2023 \
    NETCONF_MODE=tcp \
    "$C_DIR/cli-netconf-c"
}

mode_c_direct() {
    build_go     # cần mock-netconf
    build_c_direct
    start_mock_netconf   # không cần mgt-service

    echo ""
    echo -e "${GREEN}${BOLD}============================================${RESET}"
    echo -e "${GREEN}${BOLD}  C direct mode (NETCONF TCP :2023)${RESET}"
    echo -e "${GREEN}${BOLD}============================================${RESET}"
    echo ""
    echo -e "  Mock NETCONF : ${CYAN}127.0.0.1:2023 (TCP)${RESET}"
    echo ""

    NETCONF_HOST=127.0.0.1 \
    NETCONF_PORT=2023 \
    NETCONF_MODE=tcp \
    NE_NAME=mock-ne \
    "$C_DIR/cli-netconf-c-direct"
}

# ── Menu ───────────────────────────────────────────────────────
show_menu() {
    echo ""
    echo -e "${BOLD}CLI NETCONF — Local Test Setup${RESET}"
    echo -e "${DIM}──────────────────────────────────${RESET}"
    echo -e "  ${CYAN}1${RESET}  Go direct mode   (TCP :2023, không cần SSH)  ${DIM}← nhanh nhất${RESET}"
    echo -e "  ${CYAN}2${RESET}  C full mode       (mgt-service + NETCONF TCP)"
    echo -e "  ${CYAN}3${RESET}  C direct mode     (NETCONF TCP :2023, không cần mgt-service)"
    echo -e "  ${CYAN}4${RESET}  Go SSH server     (ssh admin@127.0.0.1 -p 2222)"
    echo ""
    printf "  Chọn [1-4]: "
    read -r choice
    case "$choice" in
        1) mode_go_direct ;;
        2) mode_c ;;
        3) mode_c_direct ;;
        4) mode_go_ssh ;;
        *) echo "Invalid choice"; exit 1 ;;
    esac
}

# ── Entry point ────────────────────────────────────────────────
MODE="${1:-}"
case "$MODE" in
    go-ssh)    mode_go_ssh ;;
    go-direct) mode_go_direct ;;
    c)         mode_c ;;
    c-direct)  mode_c_direct ;;
    "")        show_menu ;;
    *)
        echo "Usage: $0 [go-ssh | go-direct | c | c-direct]"
        exit 1
        ;;
esac
