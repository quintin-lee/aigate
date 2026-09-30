# aigate Performance Benchmark Suite

This directory contains the automated performance benchmarking suite for `aigate`. It measures throughput (QPS), latency percentiles (p50/p90/p99), and Time To First Token (TTFT) for streaming SSE requests using a local zero-latency mock upstream.

## Prerequisites

The benchmark harness supports **k6**, **wrk**, or the built-in multi-threaded runner:

- **k6** (Recommended for full scenario coverage and TTFT measurement):
  - Arch Linux: `sudo pacman -S k6`
  - macOS: `brew install k6`
  - Linux binary: See [k6 installation guide](https://k6.io/docs/get-started/installation/)
- **wrk** (For raw saturation and microsecond latency):
  - Arch Linux: `sudo pacman -S wrk`
  - Source build: `git clone https://github.com/wg/wrk.git && cd wrk && make && sudo cp wrk /usr/local/bin/`
- **Built-in multi-threaded runner**: Automatically used if neither `k6` nor `wrk` is found, with zero additional dependencies.

## Quick Start

Run all benchmarks using auto-detected tool:

```bash
./benchmarks/run.sh
```

Run specific scenario with custom duration and concurrency:

```bash
# Non-streaming sync chat
./benchmarks/run.sh --tool k6 --scenario sync --concurrency 50 --duration 10

# Streaming SSE chat with TTFT measurement
./benchmarks/run.sh --tool k6 --scenario stream --concurrency 20 --duration 15

# Response cache hit benchmark with wrk
./benchmarks/run.sh --tool wrk --scenario cache --concurrency 64 --duration 10
```

## Scenarios

1. **`sync`**: Standard non-streaming chat completions (`/v1/chat/completions`). Measures raw gateway proxy overhead.
2. **`stream`**: Streaming SSE chat completions. Measures TTFT and sustained streaming connection overhead.
3. **`cache`**: Cache HIT requests against `aigate`'s built-in memory response cache.
4. **`failover`**: Upstream primary node fails with 503; measures seamless failover to secondary upstream.

## Reports

Markdown benchmark reports are automatically generated under `benchmarks/reports/benchmark_<timestamp>.md`.
