# ============================================================
# Makefile — CLI NETCONF (C + MAAPI)
# ============================================================
# Phụ thuộc bắt buộc:
#   libcurl   (brew install curl  / apt install libcurl4-openssl-dev)
#   libxml2   (brew install libxml2 / apt install libxml2-dev)
#   readline  (brew install readline / apt install libreadline-dev)
#
# Phụ thuộc tuỳ chọn:
#   libssh2   (brew install libssh2) — bật WITH_SSH=1
#   libconfd  — set CONFD_DIR=/path/to/confd, bật WITH_MAAPI=1
# ============================================================

CC      := gcc
TARGET        := cli-netconf-c
TARGET_DIRECT := cli-netconf-c-direct
TARGET_MAAPI  := cli-netconf-c-maapi
SRCDIR  := src
INCDIR  := include
OBJDIR  := obj

SRCS := $(SRCDIR)/main.c      \
        $(SRCDIR)/auth.c      \
        $(SRCDIR)/netconf.c   \
        $(SRCDIR)/schema.c    \
        $(SRCDIR)/completer.c \
        $(SRCDIR)/formatter.c \
        $(SRCDIR)/commands.c

# Sources cho direct mode (auth-stub thay cho auth.c — không cần mgt-service)
DIRECT_SRCS := $(SRCDIR)/main-direct.c \
               $(SRCDIR)/auth-stub.c   \
               $(SRCDIR)/netconf.c      \
               $(SRCDIR)/schema.c       \
               $(SRCDIR)/completer.c    \
               $(SRCDIR)/formatter.c    \
               $(SRCDIR)/commands.c

# Sources cho MAAPI direct mode (cần WITH_MAAPI=1 CONFD_DIR=...)
MAAPI_SRCS := $(SRCDIR)/main-maapi.c   \
              $(SRCDIR)/maapi-ops.c     \
              $(SRCDIR)/maapi.c         \
              $(SRCDIR)/auth-stub.c     \
              $(SRCDIR)/schema.c        \
              $(SRCDIR)/completer.c     \
              $(SRCDIR)/formatter.c

# Sources cho library (không có main.c, không có completer.c — không cần readline)
LIB_SRCS := $(SRCDIR)/lib.c       \
             $(SRCDIR)/auth.c      \
             $(SRCDIR)/netconf.c   \
             $(SRCDIR)/schema.c    \
             $(SRCDIR)/formatter.c \
             $(SRCDIR)/commands.c

LIB_NAME   := libclinetconf
LIB_STATIC := $(LIB_NAME).a
LIB_SHARED := $(LIB_NAME).so

# ----- Platform detection -----
UNAME := $(shell uname -s)

ifeq ($(UNAME), Darwin)
    BREW_PREFIX  := $(shell brew --prefix 2>/dev/null || echo /usr/local)
    CURL_CFLAGS  := $(shell curl-config --cflags 2>/dev/null)
    CURL_LDFLAGS := $(shell curl-config --libs   2>/dev/null || echo -lcurl)
    XML2_CFLAGS  := $(shell xml2-config --cflags 2>/dev/null)
    XML2_LDFLAGS := $(shell xml2-config --libs   2>/dev/null || echo -lxml2)
    # readline: use brew (has rl_catch_signals, rl_replace_line)
    RL_PREFIX    := $(shell brew --prefix readline 2>/dev/null || echo /usr/local)
    RL_CFLAGS    := -I$(RL_PREFIX)/include
    # Put brew lib first so it takes precedence over macOS system readline
    RL_LDFLAGS   := -L$(RL_PREFIX)/lib -lreadline -Wl,-rpath,$(RL_PREFIX)/lib
else
    CURL_CFLAGS  := $(shell curl-config --cflags 2>/dev/null)
    CURL_LDFLAGS := $(shell curl-config --libs   2>/dev/null || echo -lcurl)
    XML2_CFLAGS  := $(shell xml2-config --cflags 2>/dev/null)
    XML2_LDFLAGS := $(shell xml2-config --libs   2>/dev/null || echo -lxml2)
    RL_CFLAGS    :=
    RL_LDFLAGS   := -lreadline
endif

# ----- Base flags -----
CFLAGS  := -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE \
           -I$(INCDIR) \
           $(CURL_CFLAGS) $(XML2_CFLAGS) $(RL_CFLAGS)
# RL_LDFLAGS first so brew readline takes precedence over macOS system readline
LDFLAGS := $(RL_LDFLAGS) $(CURL_LDFLAGS) $(XML2_LDFLAGS)

# ----- SSH support (libssh2) — tuỳ chọn -----
ifdef WITH_SSH
    SSH2_PREFIX  := $(shell brew --prefix libssh2 2>/dev/null || echo /usr)
    CFLAGS      += -DWITH_SSH -I$(SSH2_PREFIX)/include
    LDFLAGS     += -L$(SSH2_PREFIX)/lib -lssh2
endif

# ----- MAAPI (libconfd) — tuỳ chọn -----
ifdef WITH_MAAPI
    ifndef CONFD_DIR
        $(error "WITH_MAAPI=1 requires CONFD_DIR=/path/to/confd")
    endif
    SRCS    += $(SRCDIR)/maapi.c
    CFLAGS  += -DWITH_MAAPI -I$(CONFD_DIR)/include
    LDFLAGS += -L$(CONFD_DIR)/lib -lconfd
endif

OBJS        := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o,   $(SRCS))
DIRECT_OBJS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o,   $(DIRECT_SRCS))
MAAPI_OBJS  := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o,   $(MAAPI_SRCS))
LIB_OBJS    := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.lo, $(LIB_SRCS))

# ============================================================
.PHONY: all direct maapi-direct lib lib-shared clean run-tcp run-ssh run-direct run-maapi docker-build docker-build-no-ssh

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Build OK → $(TARGET)"

# ---- Direct TCP/SSH mode binary (không cần mgt-service) ----
direct: $(TARGET_DIRECT)

$(TARGET_DIRECT): $(DIRECT_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Build OK → $(TARGET_DIRECT)"

# ---- MAAPI direct mode (cần WITH_MAAPI=1 CONFD_DIR=...) ----
maapi-direct:
	$(MAKE) WITH_MAAPI=1 $(TARGET_MAAPI)

$(TARGET_MAAPI): $(MAAPI_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Build OK → $(TARGET_MAAPI)"

# ---- Static library (.a) ----
lib: $(LIB_STATIC)

$(LIB_STATIC): $(LIB_OBJS)
	ar rcs $@ $^
	@echo "Build OK → $(LIB_STATIC)"

# ---- Shared library (.so) ----
lib-shared: $(LIB_SHARED)

$(LIB_SHARED): $(LIB_OBJS)
	$(CC) -shared -o $@ $^ $(CURL_LDFLAGS) $(XML2_LDFLAGS)
	@echo "Build OK → $(LIB_SHARED)"

# ---- Object files cho executable (.o) ----
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- Object files cho library (.lo — compiled with -fPIC) ----
$(OBJDIR)/%.lo: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET) $(TARGET_DIRECT) $(TARGET_MAAPI) $(LIB_STATIC) $(LIB_SHARED)

# ----- Chạy với mock servers -----
run-direct:
	NETCONF_HOST=127.0.0.1 \
	NETCONF_PORT=2023 \
	NETCONF_MODE=tcp \
	NE_NAME=mock-ne \
	./$(TARGET_DIRECT)

run-maapi:
	CONFD_IPC_ADDR=127.0.0.1 \
	CONFD_IPC_PORT=4565 \
	NE_NAME=confd \
	./$(TARGET_MAAPI)

run-tcp:
	MGT_URL=http://127.0.0.1:3000 \
	NETCONF_HOST=127.0.0.1 \
	NETCONF_PORT=2023 \
	NETCONF_MODE=tcp \
	./$(TARGET)

run-ssh:
	MGT_URL=http://127.0.0.1:3000 \
	NETCONF_HOST=127.0.0.1 \
	NETCONF_PORT=8830 \
	NETCONF_USER=admin \
	NETCONF_PASS=admin \
	NETCONF_MODE=ssh \
	WITH_SSH=1 \
	./$(TARGET)

docker-build:
	docker build -t cli-netconf-c .

docker-build-no-ssh:
	docker build --build-arg WITH_SSH=0 -t cli-netconf-c .

.PHONY: all clean run-tcp run-ssh docker-build docker-build-ssh
