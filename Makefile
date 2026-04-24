# ============================================================
# Makefile — CLI NETCONF (C / MAAPI)
#
# Requirements:
#   libxml2   (dnf install libxml2-devel)
#   readline  (dnf install readline-devel)
#   libconfd.so — chỉ cần file .so, headers đã bundled trong include/
#
# Build:
#   make CONFD_LIB=/path/to/libconfd.so
#
#   # Nếu libconfd.so để ngay thư mục project:
#   make CONFD_LIB=./libconfd.so
#
# Run:
#   CONFD_IPC_ADDR=172.16.25.131 CONFD_IPC_PORT=4565 ./cli-netconf
# ============================================================

CC     := gcc
TARGET := cli-netconf
SRCDIR := src
INCDIR := include
OBJDIR := obj

SRCS := $(SRCDIR)/main.c       \
        $(SRCDIR)/maapi-ops.c  \
        $(SRCDIR)/maapi.c      \
        $(SRCDIR)/schema.c     \
        $(SRCDIR)/formatter.c  \
        $(SRCDIR)/json_util.c  \
        $(SRCDIR)/args_util.c  \
        $(SRCDIR)/set_plan.c   \
        $(SRCDIR)/completion_util.c \
        $(SRCDIR)/log.c

# ----- CONFD_LIB: đường dẫn đến libconfd.so -----
ifeq ($(MAKECMDGOALS),clean)
    CONFD_LIB ?= /dev/null
else
ifndef CONFD_LIB
    $(error "Cần chỉ định CONFD_LIB. Ví dụ: make CONFD_LIB=./libconfd.so")
endif
endif

# Nếu CONFD_LIB là file .so → link trực tiếp
# Nếu CONFD_LIB là thư mục  → -L dir -lconfd
CONFD_DIR := $(dir $(CONFD_LIB))
ifneq ($(suffix $(CONFD_LIB)),.so)
    CONFD_LDFLAGS := -L$(CONFD_LIB) -lconfd
else
    CONFD_LDFLAGS := $(CONFD_LIB)
endif

# libconfd.so cần libcrypto — tìm cùng thư mục với CONFD_LIB
CRYPTO_SO := $(wildcard $(CONFD_DIR)libcrypto-server.so $(CONFD_DIR)libcrypto.so.1.0.0 $(CONFD_DIR)libcrypto.so.10)
ifneq ($(CRYPTO_SO),)
    CONFD_LDFLAGS += $(firstword $(CRYPTO_SO)) -Wl,-rpath,$(CONFD_DIR)
else
    CONFD_LDFLAGS += -Wl,-rpath,$(CONFD_DIR)
endif

# ----- Platform detection -----
UNAME := $(shell uname -s)

ifeq ($(UNAME), Darwin)
    XML2_CFLAGS  := $(shell xml2-config --cflags 2>/dev/null)
    XML2_LDFLAGS := $(shell xml2-config --libs   2>/dev/null || echo -lxml2)
    RL_PREFIX    := $(shell brew --prefix readline 2>/dev/null || echo /usr/local)
    RL_CFLAGS    := -I$(RL_PREFIX)/include
    RL_LDFLAGS   := -L$(RL_PREFIX)/lib -lreadline -Wl,-rpath,$(RL_PREFIX)/lib
else
    XML2_CFLAGS  := $(shell xml2-config --cflags 2>/dev/null)
    XML2_LDFLAGS := $(shell xml2-config --libs   2>/dev/null || echo -lxml2)
    RL_CFLAGS    :=
    RL_LDFLAGS   := -lreadline
endif

CFLAGS  := -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -DWITH_MAAPI \
           -I$(INCDIR) \
           $(XML2_CFLAGS) $(RL_CFLAGS)

LDFLAGS := $(RL_LDFLAGS) $(XML2_LDFLAGS) $(CONFD_LDFLAGS)

OBJS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

# ============================================================
.PHONY: all clean run test coverage

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Build OK → $(TARGET)"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TESTDIR)/bin

run: all
	LD_LIBRARY_PATH=$$(dirname $(CONFD_LIB)):$$LD_LIBRARY_PATH \
	CONFD_IPC_ADDR=$${CONFD_IPC_ADDR:-127.0.0.1} \
	CONFD_IPC_PORT=$${CONFD_IPC_PORT:-4565} \
	./$(TARGET)

# ============================================================
# Unit tests — không cần ConfD runtime, không link libconfd.
# Chỉ test các module pure (json_util, formatter, args_util, schema).
# Gọi: make test   (CONFD_LIB vẫn cần cho Makefile parse, nhưng không link)
# ============================================================
TESTDIR := tests
TESTBIN := $(TESTDIR)/bin
TEST_CFLAGS := -Wall -Wextra -O0 -g -std=c11 -D_GNU_SOURCE \
               -I$(INCDIR) -I$(TESTDIR) $(XML2_CFLAGS)

TEST_LDFLAGS := $(XML2_LDFLAGS)

TEST_BINS := \
    $(TESTBIN)/test_json \
    $(TESTBIN)/test_formatter \
    $(TESTBIN)/test_text_to_xml \
    $(TESTBIN)/test_keypath \
    $(TESTBIN)/test_set_plan \
    $(TESTBIN)/test_completion

$(TESTBIN):
	mkdir -p $(TESTBIN)

# test_json: chỉ phụ thuộc json_util.c — không cần gì khác
$(TESTBIN)/test_json: $(TESTDIR)/test_json.c $(SRCDIR)/json_util.c | $(TESTBIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^

# test_formatter: formatter.c + schema.c + libxml2
$(TESTBIN)/test_formatter: $(TESTDIR)/test_formatter.c \
                            $(SRCDIR)/formatter.c $(SRCDIR)/schema.c | $(TESTBIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# test_text_to_xml: cùng deps với test_formatter
$(TESTBIN)/test_text_to_xml: $(TESTDIR)/test_text_to_xml.c \
                              $(SRCDIR)/formatter.c $(SRCDIR)/schema.c | $(TESTBIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# test_keypath: args_util.c + schema.c (không cần libconfd)
$(TESTBIN)/test_keypath: $(TESTDIR)/test_keypath.c \
                          $(SRCDIR)/args_util.c $(SRCDIR)/schema.c | $(TESTBIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# test_set_plan: set_plan.c + schema.c
$(TESTBIN)/test_set_plan: $(TESTDIR)/test_set_plan.c \
                           $(SRCDIR)/set_plan.c $(SRCDIR)/schema.c | $(TESTBIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# test_completion: completion_util.c + schema.c
$(TESTBIN)/test_completion: $(TESTDIR)/test_completion.c \
                             $(SRCDIR)/completion_util.c $(SRCDIR)/schema.c | $(TESTBIN)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

test: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
	    echo "▶ $$t"; \
	    $$t || fail=$$((fail+1)); \
	done; \
	echo ""; \
	if [ $$fail -eq 0 ]; then \
	    printf '\033[32mAll test binaries passed.\033[0m\n'; \
	else \
	    printf '\033[31m%d test binary(ies) had failures.\033[0m\n' $$fail; \
	    exit 1; \
	fi

# ============================================================
# Code coverage — build tests với gcov instrumentation, chạy, gen report.
# Gọi: make coverage CONFD_LIB=./libconfd-server.so
# Output: tests/bin/*.gcov (per source file) + tóm tắt %
# ============================================================
COV_FLAGS := -fprofile-arcs -ftest-coverage

coverage:
	@rm -rf $(TESTBIN)
	$(MAKE) test CONFD_LIB=$(CONFD_LIB) \
	    TEST_CFLAGS="-Wall -Wextra -O0 -g -std=c11 -D_GNU_SOURCE \
	                 -I$(INCDIR) -I$(TESTDIR) $(XML2_CFLAGS) $(COV_FLAGS)" \
	    TEST_LDFLAGS="$(XML2_LDFLAGS) $(COV_FLAGS)"
	@echo ""
	@echo "─── gcov report ──────────────────────────────────"
	@cd $(TESTBIN) && for src in json_util args_util set_plan completion_util formatter schema; do \
	    gcno=$$(ls *-$$src.gcno 2>/dev/null | head -1); \
	    [ -z "$$gcno" ] && continue; \
	    base=$${gcno%.gcno}; \
	    gcov -n "$$base" 2>/dev/null | \
	        awk -v f=$$src '/File.*'$$src'\.c/{p=1;next} p && /Lines executed/{print f": "$$0; exit}'; \
	done
