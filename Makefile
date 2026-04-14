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
        $(SRCDIR)/formatter.c

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
.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)
	@echo "Build OK → $(TARGET)"

$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET)

run: all
	LD_LIBRARY_PATH=$$(dirname $(CONFD_LIB)):$$LD_LIBRARY_PATH \
	CONFD_IPC_ADDR=$${CONFD_IPC_ADDR:-127.0.0.1} \
	CONFD_IPC_PORT=$${CONFD_IPC_PORT:-4565} \
	./$(TARGET)
