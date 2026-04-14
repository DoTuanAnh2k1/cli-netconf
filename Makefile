# ============================================================
# Makefile — CLI NETCONF (C / MAAPI)
#
# Requirements:
#   libxml2   (brew install libxml2  /  dnf install libxml2-devel)
#   readline  (brew install readline /  dnf install readline-devel)
#   libconfd  — headers + .so từ ConfD SDK
#
# Build (flexible):
#   # Nếu có đủ CONFD_DIR với include/ và lib/ bên trong:
#   make CONFD_DIR=/opt/confd
#
#   # Nếu chỉ có 2 file header + libconfd.so riêng lẻ:
#   make CONFD_INCLUDE=./include CONFD_LIB=/path/to/libconfd.so
#
#   # Header để trong include/, .so để trong project:
#   make CONFD_INCLUDE=./include CONFD_LIB=./libconfd.so
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

# ----- ConfD: hỗ trợ 2 cách chỉ định -----
ifdef CONFD_DIR
    CONFD_INCLUDE_DIR := $(CONFD_DIR)/include
    CONFD_LIB_DIR     := $(CONFD_DIR)/lib
    CONFD_LDFLAGS     := -L$(CONFD_LIB_DIR) -lconfd
else ifdef CONFD_INCLUDE
    # CONFD_INCLUDE = thư mục chứa confd_lib.h
    # CONFD_LIB    = path đến libconfd.so (hoặc thư mục chứa nó)
    CONFD_INCLUDE_DIR := $(CONFD_INCLUDE)
    ifdef CONFD_LIB
        # Nếu CONFD_LIB là file .so trực tiếp
        ifneq ($(suffix $(CONFD_LIB)),.so)
            CONFD_LDFLAGS := -L$(CONFD_LIB) -lconfd
        else
            CONFD_LDFLAGS := $(CONFD_LIB)
        endif
    else
        CONFD_LDFLAGS := -lconfd
    endif
else
    $(error "Cần chỉ định CONFD_DIR hoặc CONFD_INCLUDE. Ví dụ:\n  make CONFD_DIR=/opt/confd\n  make CONFD_INCLUDE=./include CONFD_LIB=./libconfd.so")
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
           -I$(INCDIR) -I$(CONFD_INCLUDE_DIR) \
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
	CONFD_IPC_ADDR=$${CONFD_IPC_ADDR:-127.0.0.1} \
	CONFD_IPC_PORT=$${CONFD_IPC_PORT:-4565} \
	./$(TARGET)
