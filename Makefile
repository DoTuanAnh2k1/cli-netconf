# ============================================================
# Makefile — CLI NETCONF (C / MAAPI)
#
# Requirements:
#   libxml2   (brew install libxml2  /  apt install libxml2-dev)
#   readline  (brew install readline /  apt install libreadline-dev)
#   libconfd  — set CONFD_DIR=/path/to/confd
#
# Build:
#   make CONFD_DIR=/opt/confd
#
# Run:
#   CONFD_IPC_ADDR=<host> CONFD_IPC_PORT=4565 ./cli-netconf
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

# ----- MAAPI (libconfd) — required -----
ifndef CONFD_DIR
    $(error "CONFD_DIR is required. Usage: make CONFD_DIR=/path/to/confd")
endif

CFLAGS  := -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE -DWITH_MAAPI \
           -I$(INCDIR) -I$(CONFD_DIR)/include \
           $(XML2_CFLAGS) $(RL_CFLAGS)

LDFLAGS := $(RL_LDFLAGS) $(XML2_LDFLAGS) \
           -L$(CONFD_DIR)/lib -lconfd

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
