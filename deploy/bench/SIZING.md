# aigate Capacity Planning & Sizing Guide

This document provides production capacity planning formulas, resource sizing guidelines, and operating system tuning recommendations for deploying **aigate** at scale.

---

## 1. Gateway Performance Characteristics

`aigate` is implemented in pure C11 with zero-overhead memory allocations on hot paths:
- **Event loop & Threading:** Multi-threaded CivetWeb server with configurable worker thread pool (`AIGATE_WORKER_THREADS`).
- **HTTP/SSE Client:** Asynchronous `libcurl` multi-handles supporting HTTP/1.1 and HTTP/2 pipelining to upstream providers.
- **Memory Footprint:** Base resident memory (RSS) is approximately **30MB - 50MB**. Each active concurrent streaming connection requires only ~64KB - 128KB of buffer memory.
- **Latency Overhead:** Gateway core transit overhead (routing + token validation + guardrails + metrics) is **< 1.5ms** at p99.

---

## 2. Resource Sizing Profiles

| Deployment Profile | Concurrent Streams | Target QPS | vCPU | Memory (RAM) | `AIGATE_WORKER_THREADS` | Recommended Pod Replicas |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Small (Team / Dev)** | 1 – 100 | 10 – 50 | 1 – 2 | 512 MiB | 64 | 2 (HA) |
| **Medium (Production)** | 100 – 1,000 | 50 – 500 | 2 – 4 | 1 – 2 GiB | 128 – 256 | 2 – 4 |
| **Large (Enterprise)** | 1,000 – 5,000 | 500 – 3,000 | 4 – 8 | 2 – 4 GiB | 512 | 4 – 10 |
| **Hyperscale** | 5,000+ | 3,000+ | 8+ | 4 – 8 GiB | 1024 | 10+ (with HPA) |

---

## 3. Worker Threads & In-Flight Concurrency Sizing

### Sizing Formula

CivetWeb allocates one operating system thread per active inbound HTTP connection. Long-running Server-Sent Events (SSE) connections keep a worker thread occupied for the entire duration of the generative response (e.g. 5s – 60s).

To prevent thread pool starvation:
$$\text{Worker Threads (per Pod)} \ge \frac{\text{Peak Concurrent Inbound Connections}}{\text{Replicas}} \times 1.25$$

### Upstream `max_concurrent` Formula

Each model target can specify `max_concurrent` to protect upstream GPU inference servers (vLLM, TGI, Ollama) from Out-Of-Memory (OOM) failures or avoid SaaS rate limit throttling (HTTP 429).

$$\sum_{i=1}^{M} \text{max\_concurrent}_i \le \text{Total Upstream GPU KV-cache Capacity}$$

If all targets for a model reach `max_concurrent`:
- If fallback targets exist with remaining capacity, the request is automatically routed to them.
- If all candidates are saturated, the gateway immediately returns **HTTP 429** (`Retry-After: 1`) instead of queuing unboundedly, preserving gateway responsiveness and failing fast.

---

## 4. Operating System & Kernel Network Tuning

For Linux nodes hosting high-concurrency instances of `aigate`, apply the following `/etc/sysctl.conf` configurations:

```ini
# Increase system-wide open file limit
fs.file-max = 2097152

# Increase maximum socket listen backlog (prevents TCP SYN drops during bursts)
net.core.somaxconn = 32768
net.ipv4.tcp_max_syn_backlog = 16384

# Fast reuse of TIME_WAIT sockets for outgoing connections to upstreams
net.ipv4.tcp_tw_reuse = 1

# Broaden ephemeral port range for outbound connections to providers
net.ipv4.ip_local_port_range = 1024 65535

# Increase TCP read/write buffer limits
net.core.rmem_max = 16777216
net.core.wmem_max = 16777216
net.ipv4.tcp_rmem = 4096 87380 16777216
net.ipv4.tcp_wmem = 4096 65536 16777216
```

Set file descriptor limits in `/etc/security/limits.conf`:
```text
* soft nofile 65536
* hard nofile 65536
```

---

## 5. PostgreSQL & Redis Sizing

### PostgreSQL
- **Usage & Request Logging:** `aigate` batches usage metrics in memory and flushes periodically (`AIGATE_USAGE_FLUSH_S`, default 1s).
- **Connections:** Gateway pods keep minimal persistent DB connections.
- **Recommended sizing:** 2 vCPU, 4GB RAM with SSD storage (100GB+ depending on request log retention).

### Redis
- **Role:** Distributed sliding-window rate limiting, shared circuit breaker state, admin lockout.
- **Pool Size (`AIGATE_REDIS_POOL_SIZE`):** Set to `16` – `32` per gateway pod.
- **Network Latency:** Ensure Redis is co-located with the gateway (sub-millisecond RTT, `< 1ms`).
- **Fail-open (`AIGATE_REDIS_FAIL_OPEN=1`):** Ensures AI traffic continues uninterrupted even if the Redis cluster suffers a transient outage.

---

## 6. Running Streaming Benchmarks with k6

Install [k6](https://k6.io/) and run the provided streaming load test:

```bash
k6 run \
  -e TARGET_URL="http://localhost:8080" \
  -e API_KEY="sk-your-client-api-key" \
  -e MODEL="gpt-4o" \
  deploy/bench/k6_stream_test.js
```

Observe TTFT (`llm_ttft_ms`), total stream duration, and chunk delivery stability in real-time.
