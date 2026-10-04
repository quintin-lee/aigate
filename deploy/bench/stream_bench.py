#!/usr/bin/env python3
"""
Zero-dependency SSE streaming load generator for aigate (k6 alternative).

Measures client-side TTFT (time to first content chunk), total stream
duration, chunk counts, HTTP status distribution and throughput.

Usage:
  python3 deploy/bench/stream_bench.py --url http://localhost:8080 \
      --key aig_xxx --model gpt-4o --concurrency 50 --requests 1000
"""
import argparse
import http.client
import json
import statistics
import threading
import time
import urllib.parse
from collections import Counter


def pct(data, p):
    if not data:
        return 0.0
    data = sorted(data)
    k = max(0, min(len(data) - 1, int(round(p / 100.0 * (len(data) - 1)))))
    return data[k]


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.ttft = []
        self.total = []
        self.chunks = 0
        self.status = Counter()
        self.errors = Counter()
        self.done_marker_missing = 0

    def add(self, status, ttft=None, total=None, chunks=0, done=True, err=None):
        with self.lock:
            self.status[status] += 1
            if ttft is not None:
                self.ttft.append(ttft)
            if total is not None:
                self.total.append(total)
            self.chunks += chunks
            if status == 200 and not done:
                self.done_marker_missing += 1
            if err:
                self.errors[err] += 1


def one_request(host, port, key, model, stream, vu, seq, timeout, stats):
    body = json.dumps({
        "model": model,
        "stream": stream,
        "max_tokens": 150,
        "messages": [
            {"role": "system", "content": "You are a concise, helpful assistant."},
            # unique nonce => bypass gateway response cache
            {"role": "user", "content": f"Explain SSE in 3 bullets. [req {vu}-{seq}-{time.time_ns()}]"},
        ],
    })
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {key}",
        "Accept": "text/event-stream" if stream else "application/json",
    }
    t0 = time.perf_counter()
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        conn.request("POST", "/v1/chat/completions", body=body, headers=headers)
        resp = conn.getresponse()
        if resp.status != 200:
            resp.read()
            stats.add(resp.status)
            return
        if not stream:
            resp.read()
            dt = time.perf_counter() - t0
            stats.add(200, ttft=dt, total=dt, chunks=1)
            return
        ttft = None
        chunks = 0
        done = False
        while True:
            line = resp.readline()
            if not line:
                break
            line = line.strip()
            if not line.startswith(b"data:"):
                continue
            payload = line[5:].strip()
            if payload == b"[DONE]":
                done = True
                break
            chunks += 1
            if ttft is None:
                try:
                    delta = json.loads(payload)["choices"][0].get("delta", {})
                    if delta.get("content"):
                        ttft = time.perf_counter() - t0
                except Exception:
                    pass
        total = time.perf_counter() - t0
        stats.add(200, ttft=ttft if ttft is not None else total, total=total,
                  chunks=chunks, done=done)
    except Exception as e:  # noqa: BLE001
        stats.add(-1, err=type(e).__name__)
    finally:
        conn.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://localhost:8080")
    ap.add_argument("--key", required=True)
    ap.add_argument("--model", default="gpt-4o")
    ap.add_argument("--concurrency", type=int, default=50)
    ap.add_argument("--requests", type=int, default=1000)
    ap.add_argument("--no-stream", action="store_true")
    ap.add_argument("--timeout", type=float, default=60.0)
    a = ap.parse_args()

    u = urllib.parse.urlparse(a.url)
    host, port = u.hostname, u.port or 80
    stats = Stats()
    counter = iter(range(a.requests))
    counter_lock = threading.Lock()

    def worker(vu):
        seq = 0
        while True:
            with counter_lock:
                try:
                    next(counter)
                except StopIteration:
                    return
            one_request(host, port, a.key, a.model, not a.no_stream, vu, seq, a.timeout, stats)
            seq += 1

    print(f"→ {a.requests} {'stream' if not a.no_stream else 'sync'} requests, "
          f"concurrency={a.concurrency}, target={a.url}, model={a.model}")
    t0 = time.perf_counter()
    threads = [threading.Thread(target=worker, args=(i,), daemon=True) for i in range(a.concurrency)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall = time.perf_counter() - t0

    ok = stats.status.get(200, 0)
    n = sum(stats.status.values())
    ms = lambda v: f"{v * 1000:8.1f} ms"  # noqa: E731
    print("\n================ aigate stream benchmark ================")
    print(f"wall time        : {wall:.2f} s")
    print(f"requests         : {n}  (ok={ok}, success={ok / max(n, 1) * 100:.2f}%)")
    print(f"throughput       : {n / wall:.1f} req/s")
    print(f"status codes     : {dict(stats.status)}")
    if stats.errors:
        print(f"client errors    : {dict(stats.errors)}")
    if stats.done_marker_missing:
        print(f"missing [DONE]   : {stats.done_marker_missing}")
    print(f"chunks received  : {stats.chunks}")
    if stats.ttft:
        print("---------------- TTFT (client) ----------------")
        print(f"  avg {ms(statistics.mean(stats.ttft))} | p50 {ms(pct(stats.ttft, 50))} | "
              f"p95 {ms(pct(stats.ttft, 95))} | p99 {ms(pct(stats.ttft, 99))} | max {ms(max(stats.ttft))}")
    if stats.total:
        print("---------------- Total stream duration --------")
        print(f"  avg {ms(statistics.mean(stats.total))} | p50 {ms(pct(stats.total, 50))} | "
              f"p95 {ms(pct(stats.total, 95))} | p99 {ms(pct(stats.total, 99))} | max {ms(max(stats.total))}")
    print("==========================================================")


if __name__ == "__main__":
    main()
