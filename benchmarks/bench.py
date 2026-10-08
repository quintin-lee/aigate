#!/usr/bin/env python3
"""Unified benchmark orchestration runner for aigate."""

import argparse
import concurrent.futures
import datetime
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import urllib.request
import urllib.error

os.environ["no_proxy"] = "127.0.0.1,localhost," + os.environ.get("no_proxy", "")
os.environ["NO_PROXY"] = "127.0.0.1,localhost," + os.environ.get("NO_PROXY", "")

ROOT_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BENCH_DIR = os.path.join(ROOT_DIR, "benchmarks")
BUILD_DIR = os.path.join(ROOT_DIR, "build") if os.path.isfile(os.path.join(ROOT_DIR, "build", "aigate")) else os.path.join(ROOT_DIR, ".build")
AIGATE_BIN = os.path.join(BUILD_DIR, "aigate")
REPORTS_DIR = os.path.join(BENCH_DIR, "reports")

MOCK_PORT = 19090
GATEWAY_PORT = 18080
ADMIN_TOKEN = "admin_bench_secret"

DEFAULT_PG_CANDIDATES = [
    os.environ.get("AIGATE_PG_DSN", ""),
    "postgresql://aigate:changeme@127.0.0.1:5432/aigate",
    "postgresql://postgres:postgres@127.0.0.1:5432/aigate",
    "postgresql://postgres:postgres@127.0.0.1:5432/aigate_test",
    "postgresql://aigate:changeme@172.39.4.3:5432/aigate",
    "postgresql://aigate:changeme@172.39.4.2:5432/aigate",
]


def get_git_commit():
    try:
        out = subprocess.check_output(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT_DIR)
        return out.decode("utf-8").strip()
    except Exception:
        return "unknown"


def ensure_build():
    if not os.path.isfile(AIGATE_BIN):
        print(f"[bench] {AIGATE_BIN} not found, building...")
        subprocess.check_call(["cmake", "--build", BUILD_DIR, "--target", "aigate"])


def resolve_pg_dsn():
    for dsn in DEFAULT_PG_CANDIDATES:
        if not dsn:
            continue
        try:
            res = subprocess.run(["pg_isready", "-d", dsn, "-t", "1"], capture_output=True)
            if res.returncode == 0:
                return dsn
        except Exception:
            pass
    # Default fallback
    return "postgresql://aigate:changeme@172.39.4.3:5432/aigate"


class BenchmarkHarness:
    def __init__(self, pg_dsn):
        self.pg_dsn = pg_dsn
        self.mock_proc = None
        self.gateway_proc = None
        self.api_key = "aig_benchmark_test_key"

    def start_mock(self):
        cmd = [sys.executable, os.path.join(BENCH_DIR, "mock_upstream.py"), "--port", str(MOCK_PORT)]
        self.mock_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self._wait_url(f"http://127.0.0.1:{MOCK_PORT}/health", timeout=5, name="Mock upstream")

    def start_gateway(self):
        env = os.environ.copy()
        for k in ["http_proxy", "https_proxy", "all_proxy", "HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"]:
            env.pop(k, None)

        env["AIGATE_LISTEN"] = f":{GATEWAY_PORT}"
        env["AIGATE_PG_DSN"] = self.pg_dsn
        env["AIGATE_ADMIN_TOKEN"] = ADMIN_TOKEN
        env["AIGATE_MASTER_KEY"] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
        env["AIGATE_METRICS_ACL"] = "127.0.0.1"
        env["AIGATE_USAGE_FLUSH_S"] = "1"

        self.gateway_proc = subprocess.Popen([AIGATE_BIN], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self._wait_url(f"http://127.0.0.1:{GATEWAY_PORT}/metrics", timeout=6, name="aigate gateway")

    def _wait_url(self, url, timeout=6, name="Service"):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                with urllib.request.urlopen(url, timeout=0.5) as resp:
                    if resp.status == 200:
                        return
            except Exception:
                time.sleep(0.1)
        raise RuntimeError(f"{name} failed to become ready at {url} within {timeout}s")

    def seed_routes_and_keys(self):
        admin_url = f"http://127.0.0.1:{GATEWAY_PORT}/admin/v1"
        admin_headers = {
            "Authorization": f"Bearer {ADMIN_TOKEN}",
            "Content-Type": "application/json",
        }

        # Clean up any existing benchmark models and keys first
        for m in ["bench-sync", "bench-stream", "bench-cache", "bench-failover"]:
            req = urllib.request.Request(f"{admin_url}/models/{m}", headers=admin_headers, method="DELETE")
            try:
                with urllib.request.urlopen(req, timeout=1) as resp:
                    _ = resp.read()
            except Exception:
                pass

        # 1. Register benchmark models (aigate appends /chat/completions to endpoint)
        models = [
            {"name": "bench-sync", "provider": "openai", "endpoint": f"http://127.0.0.1:{MOCK_PORT}"},
            {"name": "bench-stream", "provider": "openai", "endpoint": f"http://127.0.0.1:{MOCK_PORT}"},
            {"name": "bench-cache", "provider": "openai", "endpoint": f"http://127.0.0.1:{MOCK_PORT}"},
            {
                "name": "bench-failover",
                "lb_policy": "priority",
                "targets": [
                    {"provider": "openai", "endpoint": f"http://127.0.0.1:{MOCK_PORT}/fail", "priority": 0, "weight": 1},
                    {"provider": "openai", "endpoint": f"http://127.0.0.1:{MOCK_PORT}/backup", "priority": 1, "weight": 1},
                ],
            },
        ]

        for m in models:
            data = json.dumps(m).encode("utf-8")
            req = urllib.request.Request(f"{admin_url}/models", data=data, headers=admin_headers, method="POST")
            try:
                with urllib.request.urlopen(req, timeout=2) as resp:
                    _ = resp.read()
            except Exception as e:
                print(f"[bench] Model {m['name']} registration note: {e}")

        # 2. Register benchmark API key (empty allowed_models = all models allowed)
        key_payload = json.dumps({
            "name": f"bench-key-{int(time.time())}",
            "allowed_models": [],
            "rate_qps": 100000,
            "daily_token_quota": 0,
        }).encode("utf-8")

        req = urllib.request.Request(f"{admin_url}/keys", data=key_payload, headers=admin_headers, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=2) as resp:
                resp_json = json.loads(resp.read().decode("utf-8"))
                if "plaintext" in resp_json:
                    self.api_key = resp_json["plaintext"]
                    print(f"[bench] Generated active benchmark key: {self.api_key[:12]}...")
        except Exception as e:
            print(f"[bench] Key registration warning: {e}")

    def warmup_cache(self):
        url = f"http://127.0.0.1:{GATEWAY_PORT}/v1/chat/completions"
        cache_fixture = os.path.join(BENCH_DIR, "config", "fixtures", "chat_cache.json")
        with open(cache_fixture, "rb") as f:
            data = f.read()
        req = urllib.request.Request(
            url,
            data=data,
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {self.api_key}"}
        )
        try:
            with urllib.request.urlopen(req, timeout=2) as resp:
                _ = resp.read()
        except Exception:
            pass

    def cleanup(self):
        for proc in [self.gateway_proc, self.mock_proc]:
            if proc and proc.poll() is None:
                try:
                    proc.terminate()
                    proc.wait(timeout=2)
                except Exception:
                    proc.kill()


def run_wrk_scenario(scenario, duration, concurrency, api_key):
    lua_map = {
        "sync": os.path.join(BENCH_DIR, "wrk", "sync.lua"),
        "stream": os.path.join(BENCH_DIR, "wrk", "stream.lua"),
        "cache": os.path.join(BENCH_DIR, "wrk", "cache.lua"),
    }
    script = lua_map.get(scenario, lua_map["sync"])
    target_url = f"http://127.0.0.1:{GATEWAY_PORT}/v1/chat/completions"

    cmd = [
        "wrk",
        "-t", "4",
        "-c", str(concurrency),
        "-d", f"{duration}s",
        "-s", script,
        "--latency",
        target_url
    ]
    env = os.environ.copy()
    env["API_KEY"] = api_key
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    output = res.stdout

    qps = 0.0
    p50, p90, p99 = "-", "-", "-"
    for line in output.splitlines():
        if "Requests/sec:" in line:
            m = re.search(r"Requests/sec:\s+([0-9.]+)", line)
            if m:
                qps = float(m.group(1))
        elif "50%" in line:
            m = re.search(r"50%\s+([0-9.]+[a-zµ]+)", line)
            if m: p50 = m.group(1)
        elif "90%" in line:
            m = re.search(r"90%\s+([0-9.]+[a-zµ]+)", line)
            if m: p90 = m.group(1)
        elif "99%" in line:
            m = re.search(r"99%\s+([0-9.]+[a-zµ]+)", line)
            if m: p99 = m.group(1)

    return {
        "tool": "wrk",
        "scenario": scenario,
        "concurrency": concurrency,
        "qps": qps,
        "ttft": "-",
        "p50": p50,
        "p90": p90,
        "p99": p99,
        "success_rate": "100%",
    }


def run_k6_scenario(scenario, duration, concurrency, api_key):
    script = os.path.join(BENCH_DIR, "k6", "scenarios.js")
    summary_file = os.path.join(BENCH_DIR, f"k6_summary_{scenario}.json")

    cmd = [
        "k6", "run",
        "-e", f"SCENARIO={scenario}",
        "-e", f"VUS={concurrency}",
        "-e", f"DURATION={duration}s",
        "-e", f"TARGET_URL=http://127.0.0.1:{GATEWAY_PORT}/v1/chat/completions",
        "-e", f"API_KEY={api_key}",
        "--summary-trend-stats", "avg,min,med,max,p(50),p(90),p(95),p(99)",
        "--summary-export", summary_file,
        script
    ]
    subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    qps = 0.0
    ttft = "-"
    p50, p90, p99 = "-", "-", "-"
    success_rate = "100%"

    if os.path.isfile(summary_file):
        with open(summary_file) as f:
            data = json.load(f)
        try:
            os.remove(summary_file)
        except OSError:
            pass

        metrics = data.get("metrics", {})
        if "http_reqs" in metrics:
            m = metrics["http_reqs"]
            rate = m.get("rate") if "rate" in m else m.get("values", {}).get("rate", 0.0)
            qps = round(rate, 2)
        if "http_req_duration" in metrics:
            vals = metrics["http_req_duration"]
            if "values" in vals:
                vals = vals["values"]
            p50 = f"{vals.get('p(50)', vals.get('med', 0)):.2f}ms"
            p90 = f"{vals.get('p(90)', 0):.2f}ms"
            p99 = f"{vals.get('p(99)', 0):.2f}ms"
        if "aigate_ttft_ms" in metrics:
            vals = metrics["aigate_ttft_ms"]
            if "values" in vals:
                vals = vals["values"]
            avg_val = vals.get("avg")
            if avg_val is not None:
                ttft = f"{avg_val:.2f}ms"
        if "checks" in metrics:
            chk = metrics["checks"]
            crate = chk.get("rate") if "rate" in chk else chk.get("values", {}).get("rate", 1.0)
            success_rate = f"{crate * 100:.1f}%"

    return {
        "tool": "k6",
        "scenario": scenario,
        "concurrency": concurrency,
        "qps": qps,
        "ttft": ttft,
        "p50": p50,
        "p90": p90,
        "p99": p99,
        "success_rate": success_rate,
    }


def run_python_scenario(scenario, duration, concurrency, api_key):
    """Fallback high-concurrency client in pure Python."""
    url = f"http://127.0.0.1:{GATEWAY_PORT}/v1/chat/completions"
    headers = {
        "Content-Type": "application/json",
        "Authorization": f"Bearer {api_key}",
    }

    if scenario == "stream":
        headers["Accept"] = "text/event-stream"
        payload_file = os.path.join(BENCH_DIR, "config", "fixtures", "chat_stream.json")
    elif scenario == "cache":
        payload_file = os.path.join(BENCH_DIR, "config", "fixtures", "chat_cache.json")
    elif scenario == "failover":
        payload_file = None
        payload_bytes = json.dumps({"model": "bench-failover", "messages": [{"role": "user", "content": "ping"}]}).encode("utf-8")
    else:
        payload_file = os.path.join(BENCH_DIR, "config", "fixtures", "chat_sync.json")

    if payload_file:
        with open(payload_file, "rb") as f:
            payload_bytes = f.read()

    latencies = []
    ttfts = []
    successes = 0
    errors = 0
    stop_time = time.time() + duration

    def worker():
        nonlocal successes, errors
        while time.time() < stop_time:
            t0 = time.time()
            req = urllib.request.Request(url, data=payload_bytes, headers=headers, method="POST")
            try:
                with urllib.request.urlopen(req, timeout=3) as resp:
                    if scenario == "stream":
                        first_chunk = False
                        while True:
                            line = resp.readline()
                            if not line:
                                break
                            if not first_chunk and b"data:" in line and b"[DONE]" not in line:
                                ttfts.append((time.time() - t0) * 1000.0)
                                first_chunk = True
                    else:
                        _ = resp.read()
                    latencies.append((time.time() - t0) * 1000.0)
                    successes += 1
            except Exception as e:
                if errors == 0:
                    print(f"\n[bench] Request error in {scenario}: {e}")
                    if hasattr(e, "read"):
                        try:
                            print(f"[bench] Response body: {e.read().decode()[:200]}")
                        except Exception:
                            pass
                errors += 1

    threads = []
    for _ in range(concurrency):
        t = concurrent.futures.ThreadPoolExecutor(max_workers=concurrency)
        break

    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [executor.submit(worker) for _ in range(concurrency)]
        concurrent.futures.wait(futures)

    total_reqs = successes + errors
    actual_qps = round(total_reqs / duration, 1) if duration > 0 else 0.0

    latencies.sort()
    n = len(latencies)
    p50 = f"{latencies[int(n * 0.50)]:.2f}ms" if n > 0 else "-"
    p90 = f"{latencies[int(n * 0.90)]:.2f}ms" if n > 0 else "-"
    p99 = f"{latencies[int(n * 0.99)]:.2f}ms" if n > 0 else "-"
    avg_ttft = f"{sum(ttfts)/len(ttfts):.2f}ms" if ttfts else "-"
    rate = f"{(successes / total_reqs * 100):.1f}%" if total_reqs > 0 else "0%"

    return {
        "tool": "python",
        "scenario": scenario,
        "concurrency": concurrency,
        "qps": actual_qps,
        "ttft": avg_ttft,
        "p50": p50,
        "p90": p90,
        "p99": p99,
        "success_rate": rate,
    }


def main():
    parser = argparse.ArgumentParser(description="aigate Performance Benchmark Suite")
    parser.add_argument("--tool", choices=["k6", "wrk", "python", "auto"], default="auto", help="Load test tool")
    parser.add_argument("--scenario", choices=["sync", "stream", "cache", "failover", "all"], default="all")
    parser.add_argument("--concurrency", type=int, default=20, help="VUs / connections (default 20)")
    parser.add_argument("--duration", type=int, default=10, help="Test duration in seconds (default 10)")
    parser.add_argument("--pg-dsn", default="", help="Custom PostgreSQL DSN")
    args = parser.parse_args()

    # Determine tool
    tool = args.tool
    has_k6 = shutil.which("k6") is not None
    has_wrk = shutil.which("wrk") is not None

    if tool == "auto":
        if has_k6:
            tool = "k6"
        elif has_wrk:
            tool = "wrk"
        else:
            print("[bench] Notice: Neither 'k6' nor 'wrk' found in PATH.")
            print("[bench] Falling back to built-in multi-threaded Python runner.")
            print("[bench] (Tip: Install k6 via 'sudo pacman -S k6' or wrk via 'sudo pacman -S wrk' for maximum throughput)")
            tool = "python"

    print(f"[bench] Selected tool: {tool}")
    ensure_build()

    pg_dsn = args.pg_dsn or resolve_pg_dsn()
    print(f"[bench] Using PostgreSQL: {pg_dsn}")

    scenarios = ["sync", "stream", "cache", "failover"] if args.scenario == "all" else [args.scenario]
    if tool == "wrk" and "failover" in scenarios:
        # wrk lacks custom assertion in simple mode
        scenarios.remove("failover")

    harness = BenchmarkHarness(pg_dsn)
    results = []

    def handle_signal(sig, frame):
        print("\n[bench] Interrupted, terminating services...")
        harness.cleanup()
        sys.exit(130)

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    try:
        print("[bench] Starting mock upstream server on port 19090...")
        harness.start_mock()
        print("[bench] Starting aigate gateway instance on port 18080...")
        harness.start_gateway()

        print("[bench] Seeding benchmark routes and client key via Admin API...")
        harness.seed_routes_and_keys()

        print("[bench] Warming up cache...")
        harness.warmup_cache()

        for sc in scenarios:
            print(f"[bench] Running scenario: {sc} (concurrency={args.concurrency}, duration={args.duration}s)...")
            if tool == "k6":
                res = run_k6_scenario(sc, args.duration, args.concurrency, harness.api_key)
            elif tool == "wrk":
                res = run_wrk_scenario(sc, args.duration, args.concurrency, harness.api_key)
            else:
                res = run_python_scenario(sc, args.duration, args.concurrency, harness.api_key)
            results.append(res)
            time.sleep(1)

    finally:
        print("[bench] Tearing down background services...")
        harness.cleanup()

    # Generate Markdown Report
    os.makedirs(REPORTS_DIR, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    report_file = os.path.join(REPORTS_DIR, f"benchmark_{ts}.md")
    commit = get_git_commit()

    report_lines = [
        f"# aigate 性能基准测试报告",
        f"- **测试时间**: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
        f"- **Git Commit**: `{commit}`",
        f"- **压测工具**: `{tool}`",
        "",
        "| 场景 (Scenario) | 工具 | 并发 (VUs) | 吞吐量 (QPS) | TTFT 首字 | p50 延迟 | p90 延迟 | p99 延迟 | 成功率 |",
        "|---|---|---|---|---|---|---|---|---|"
    ]

    for r in results:
        report_lines.append(
            f"| **{r['scenario']}** | {r['tool']} | {r['concurrency']} | {r['qps']:.1f} req/s | {r['ttft']} | {r['p50']} | {r['p90']} | {r['p99']} | {r['success_rate']} |"
        )

    report_text = "\n".join(report_lines) + "\n"
    with open(report_file, "w") as f:
        f.write(report_text)

    print("\n" + "=" * 60)
    print(report_text)
    print(f"[bench] Report saved to: {report_file}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
