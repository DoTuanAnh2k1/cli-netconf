# ── Build stage ───────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

# CONFD_DIR must point to a ConfD installation with headers and libs
ARG CONFD_DIR=/opt/confd

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc \
        make \
        libxml2-dev \
        libreadline-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN make CONFD_DIR=${CONFD_DIR}

# ── Final stage ───────────────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        libxml2 \
        libreadline8 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /build/cli-netconf ./cli-netconf

# Configuration via env vars
ENV CONFD_IPC_ADDR=127.0.0.1 \
    CONFD_IPC_PORT=4565 \
    MAAPI_USER=admin \
    NE_NAME=confd

ENTRYPOINT ["./cli-netconf"]
