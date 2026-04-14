# ============================================================
# Root Makefile — CLI NETCONF
#
# Hai implementation:
#   go/   — Go implementation (SSH server + direct mode)
#   c/    — C implementation  (direct mode, MAAPI optional)
# ============================================================

.PHONY: all build-go build-c clean-go clean-c clean test-go run-go run-c docker-build-c

all: build-go build-c

# ---- Go ----

build-go:
	$(MAKE) -C go build-all

clean-go:
	$(MAKE) -C go clean

test-go:
	$(MAKE) -C go test

run-go:
	$(MAKE) -C go run-direct

# ---- C ----

build-c:
	$(MAKE) -C c

build-c-direct:
	$(MAKE) -C c direct

clean-c:
	$(MAKE) -C c clean

run-c:
	$(MAKE) -C c run-tcp

run-c-direct:
	$(MAKE) -C c run-direct

docker-build-c:
	$(MAKE) -C c docker-build

# ---- Combined ----

clean: clean-go clean-c

test: test-go
