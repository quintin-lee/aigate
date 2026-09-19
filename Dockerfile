# ── Stage 1: Build ───────────────────────────────────────────────
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake pkg-config ca-certificates \
        libcurl4-openssl-dev libssl-dev libpq-dev libjansson-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY src/ src/
COPY tests/ tests/

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" --target aigate

# ── Stage 2: Runtime ────────────────────────────────────────────
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 libssl3 libpq5 libjansson4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd -r aigate && useradd -r -g aigate -s /usr/sbin/nologin aigate

COPY --from=builder /src/build/aigate /usr/local/bin/aigate

USER aigate
EXPOSE 8080

ENTRYPOINT ["aigate"]
