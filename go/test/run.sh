#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Colors
GREEN='\033[32m'
YELLOW='\033[33m'
CYAN='\033[36m'
RESET='\033[0m'

cleanup() {
    echo -e "\n${YELLOW}Stopping all services...${RESET}"
    [ -n "$PID_MGT" ] && kill "$PID_MGT" 2>/dev/null || true
    [ -n "$PID_NC" ] && kill "$PID_NC" 2>/dev/null || true
    [ -n "$PID_CLI" ] && kill "$PID_CLI" 2>/dev/null || true
    wait 2>/dev/null
    echo -e "${GREEN}Done.${RESET}"
}
trap cleanup EXIT

cd "$PROJECT_DIR"

echo -e "${CYAN}Building all binaries...${RESET}"
go build -o bin/cli-netconf      ./cmd/netconf
go build -o bin/mock-mgt         ./test/mock-mgt
go build -o bin/mock-netconf     ./test/mock-netconf

echo ""
echo -e "${CYAN}Starting mock mgt-service on :3000...${RESET}"
./bin/mock-mgt &
PID_MGT=$!
sleep 0.5

echo -e "${CYAN}Starting mock NETCONF server on :8830...${RESET}"
./bin/mock-netconf &
PID_NC=$!
sleep 0.5

echo -e "${CYAN}Starting CLI SSH server on :2222...${RESET}"
MGT_SERVICE_URL=http://127.0.0.1:3000 \
NETCONF_USERNAME=admin \
NETCONF_PASSWORD=admin \
./bin/cli-netconf &
PID_CLI=$!
sleep 0.5

echo ""
echo -e "${GREEN}============================================${RESET}"
echo -e "${GREEN}  All services running!${RESET}"
echo -e "${GREEN}============================================${RESET}"
echo ""
echo -e "  Mock mgt-service:   ${CYAN}http://127.0.0.1:3000${RESET}  (PID $PID_MGT)"
echo -e "  Mock NETCONF:       ${CYAN}127.0.0.1:8830${RESET}         (PID $PID_NC)"
echo -e "  CLI SSH server:     ${CYAN}127.0.0.1:2222${RESET}         (PID $PID_CLI)"
echo ""
echo -e "  ${YELLOW}Connect with:${RESET}"
echo -e "    ssh admin@127.0.0.1 -p 2222"
echo -e "    password: admin"
echo ""
echo -e "  ${YELLOW}Test commands:${RESET}"
echo -e "    show ne"
echo -e "    connect 1"
echo -e "    get-config"
echo -e "    get-config /system"
echo -e "    edit-config"
echo -e "    commit"
echo -e "    disconnect"
echo -e "    exit"
echo ""
echo -e "  Press ${YELLOW}Ctrl+C${RESET} to stop all services."
echo ""

wait
