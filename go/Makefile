# ============================================================
# Makefile — CLI NETCONF (Go implementation)
# ============================================================

BIN := bin

.PHONY: all build build-all clean test run run-direct docker-build generate-host-key

all: build-all

# --- Build targets ---

build-all: $(BIN)/cli-netconf $(BIN)/cli-direct $(BIN)/mock-mgt $(BIN)/mock-netconf

$(BIN)/cli-netconf:
	mkdir -p $(BIN)
	go build -o $@ ./cmd/netconf

$(BIN)/cli-direct:
	mkdir -p $(BIN)
	go build -o $@ ./cmd/direct

$(BIN)/cli-direct-ssh:
	mkdir -p $(BIN)
	go build -o $@ ./cmd/direct-ssh

$(BIN)/cli-direct-tcp:
	mkdir -p $(BIN)
	go build -o $@ ./cmd/direct-tcp

$(BIN)/mock-mgt:
	mkdir -p $(BIN)
	go build -o $@ ./test/mock-mgt

$(BIN)/mock-netconf:
	mkdir -p $(BIN)
	go build -o $@ ./test/mock-netconf

# Alias
build: $(BIN)/cli-netconf

# --- Run ---

run: $(BIN)/cli-netconf
	MGT_SERVICE_URL=http://127.0.0.1:3000 ./$(BIN)/cli-netconf

run-direct: $(BIN)/cli-direct
	MGT_URL=http://127.0.0.1:3000 \
	NETCONF_HOST=127.0.0.1 \
	NETCONF_PORT=2023 \
	NETCONF_MODE=tcp \
	./$(BIN)/cli-direct

# --- Test ---

test: $(BIN)/cli-netconf $(BIN)/mock-mgt $(BIN)/mock-netconf
	go test -v -timeout 300s ./test/

# --- Misc ---

clean:
	rm -rf $(BIN)/

docker-build:
	docker build -t cli-netconf .

generate-host-key:
	ssh-keygen -t ed25519 -f host_key -N ""
