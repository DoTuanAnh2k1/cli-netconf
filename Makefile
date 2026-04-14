# ============================================================
# Root Makefile — CLI NETCONF
#
# Hai implementation:
#   go/   — Go implementation (SSH server + direct mode)
#   c/    — C implementation  (direct mode, MAAPI optional)
# ============================================================

.PHONY: all build-go build-c clean-go clean-c clean test-go run-go run-c

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

clean-c:
	$(MAKE) -C c clean

run-c:
	$(MAKE) -C c run-tcp

# ---- Combined ----

clean: clean-go clean-c

test: test-go
