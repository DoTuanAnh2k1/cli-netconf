# ── Build stage ───────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

# WITH_SSH=1 để bật NETCONF over SSH (cần libssh2)
ARG WITH_SSH=1

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc \
        make \
        libcurl4-openssl-dev \
        libxml2-dev \
        libreadline-dev \
        libssh2-1-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN if [ "$WITH_SSH" = "1" ]; then \
        make WITH_SSH=1; \
    else \
        make; \
    fi

# ── Final stage ───────────────────────────────────────────────────────────────
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 \
        libxml2 \
        libreadline8 \
        libssh2-1 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /build/cli-netconf-c ./cli-netconf-c

# Cấu hình qua env vars
ENV MGT_URL=http://mgt-service:3000 \
    NETCONF_MODE=tcp \
    NETCONF_USER=admin \
    NETCONF_PASS=admin

ENTRYPOINT ["./cli-netconf-c"]
