# 混沌故障注入与 ASan/UBSan 健壮性测试套件设计规范

- **状态**: Approved
- **日期**: 2026-09-30
- **目标**: 为 aigate 构建生产级工程健壮性保障体系，包括底层 TCP/HTTP 混沌故障注入测试套件（覆盖慢速滴灌、中途断连、客户端取消、畸形损坏数据）与 AddressSanitizer (ASan) / UndefinedBehaviorSanitizer (UBSan) 自动化内存与未定义行为检测。

---

## 1. 背景与目标

在高并发与不可靠网络环境下，AI 网关常面临慢上游推理拖垮连接池、流式传输中途网络瞬断、客户端提前关闭连接导致孤儿请求、以及上游输出畸形报文导致解析崩溃等挑战。

本项目在完成核心管道解耦与基准压测套件后，需要引入系统化的混沌工程与内存安全校验手段，达成以下目标：

1. **极端异常与网络抖动防御**：通过精确到 TCP 字节流与套接字级别的模拟注入，验证网关在慢速滴灌（Slowloris）、网络突发挂断、客户端主动取消流式请求等恶劣场景下的鲁棒性。
2. **畸形数据与边界防渗透**：模拟上游输出非法 Chunked 编码、截断 JSON、超长请求体，验证网关防御性编程与错误状态码映射机制。
3. **C 语言内存与并发安全清零**：在开启 AddressSanitizer、LeakSanitizer 与 UndefinedBehaviorSanitizer 的高敏感环境下跑通全量混沌用例，确保 **零内存泄漏、零野指针/悬挂指针使用、零缓冲区溢出、零未定义行为**。
4. **一键自动化集成**：提供即开即用的独立测试脚本与 CI 验证入口，支持日常极速回归与完整 ASan 沙箱构建检测。

---

## 2. 总体架构与目录结构

所有混沌测试相关资源集中于 `tests/chaos/`，一键构建与内存检测脚本放置于 `scripts/`。

```text
aigate/
├── tests/
│   └── chaos/
│       ├── README.md               # 混沌测试使用说明与场景指南
│       ├── chaos_server.py         # 原生底层 Socket 故障注入上游服务
│       ├── test_chaos.py           # 混沌场景编排与断言测试套件
│       └── fixtures/               # 静态畸形测试样本
│           ├── invalid_utf8.bin    # 畸形非法 UTF-8 字节流
│           ├── malformed_json.txt  # 语法截断的非合法 JSON
│           └── giant_body.txt      # 超大请求体测试桩
└── scripts/
    └── run_chaos_asan.sh           # ASan/UBSan 一键自动化编译与全量检测脚本
```

### 架构调用拓扑

```text
┌───────────────────────────┐         HTTP/1.1          ┌───────────────────────────┐
│     Client Test Suite     │ ────────────────────────> │        aigate Core        │
│    (tests/chaos/          │ <─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─  │  (Build: -fsanitize=     │
│     test_chaos.py)        │      (Stream / SSE)       │   address,undefined)      │
└───────────────────────────┘                           └───────────────────────────┘
              │                                                       │
              │ Client Abort (RST / FIN)                              │ libcurl
              ▼                                                       ▼
┌───────────────────────────┐                           ┌───────────────────────────┐
│   Early Socket Teardown   │                           │     Chaos Upstream Mock   │
│ (Verifies gateway cleans  │                           │ (tests/chaos/             │
│  up upstream curl handle) │                           │  chaos_server.py)         │
└───────────────────────────┘                           │  • Slowloris byte trickle │
                                                        │  • Sudden TCP drop        │
                                                        │  • Corrupted chunks       │
                                                        └───────────────────────────┘
```

---

## 3. 混沌故障注入场景矩阵 (Chaos Scenarios)

| 场景编号 | 场景代号 | 故障注入行为 (Chaos Action) | 预期网关响应 / 行为 |
|---|---|---|---|
| **C-01** | `slow-header` | 上游在收到请求后，以每秒 1 个字符的极慢速率发送 HTTP Response Header | 网关上游超时机制介入，返回 `504 Gateway Timeout`，并完全释放内部会话上下文 |
| **C-02** | `slow-stream` | 上游返回标准 SSE 响应头，但在推送 SSE 数据块时，每个 chunk 间隔 0.5s，模拟长尾卡顿 | 网关流式转发保持平稳，Worker 线程正常流转，客户端逐字接收，超时配置正常约束 |
| **C-03** | `upstream-drop` | 上游在输出 2 个合法 SSE chunk 后，直接通过底层套接字执行 `close()` 或发送 `TCP RST` | 网关 libcurl 感知上游断开，向客户端平稳结束连接，内部结构体即刻析构，无悬挂指针 |
| **C-04** | `client-abort` | 客户端发起 SSE 请求，收到第 1 个 chunk 后立即主动 `close()` 客户端底层套接字 | 网关写操作感知 `EPIPE/ECONNRESET`，立即中止上游 libcurl 传输，释放连接句柄 |
| **C-05** | `bad-chunked` | 上游声明 `Transfer-Encoding: chunked`，但在传输过程中发送非法 chunk 长度或乱码 | 网关捕获传输层解析错误，返回 `502 Bad Gateway`，避免内存越界与缓冲区溢出 |
| **C-06** | `truncated-json` | 上游返回 HTTP 200，但响应体在 JSON 字段中间被硬截断 | 网关协议转换与 JSON 解析逻辑优雅失败，向客户端返回标准 `502 Bad Gateway` |
| **C-07** | `oversized-body` | 客户端发送超大请求体（如 20MB+）冲击网关 | 网关前置流控/尺寸检查阻断请求，返回 `413 Payload Too Large`，无内存溢出与 OOM |

---

## 4. 核心组件详细设计

### 4.1 混沌服务端 (`tests/chaos/chaos_server.py`)

采用 Python 原生 `socketserver.ThreadingTCPServer`，不依赖第三方 Web 框架，获得完全的底层 Socket 控制力。

1. **协议路由与控制机制**：
   - 监听本地端口（默认 `19098`）。
   - 支持通过请求 URL 路径（如 `/chaos/drop-stream`）或自定义 Header `X-Chaos-Action: drop-stream` 触发特定故障。
2. **故障行为实现**：
   - **`slow-header`**：写入 `HTTP/1.1 200 OK\r\n` 后，逐字节 `time.sleep(0.3)` 发送，直到连接被网关超时切断。
   - **`slow-stream`**：输出标准 `Content-Type: text/event-stream`，循环发送包含 payload 的 chunk，每个 chunk 暂停 0.4s。
   - **`drop-stream`**：输出响应头与前 2 个合法 token 事件，使用 `sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))` 强制发送 TCP RST 或直接 `sock.close()`。
   - **`bad-chunked`**：发送首个标准分块后，发送形如 `XYZ\r\nCORRUPTED_BYTES\r\n` 的非法报文。
   - **`truncated-json`**：发送 `Content-Length: 120`，但实际仅发送 40 字节不完整 JSON 即结束连接。
   - **`/health`**：常规 HTTP 200 健康探测响应，供网关测试前快速存活确认。

### 4.2 自动化测试套件 (`tests/chaos/test_chaos.py`)

基于 Python 编写的完整编排与断言套件，具备双重执行模式（独立自包含启动网关模式 / 外部已运行网关接入模式）：

1. **环境准备与自动发现**：
   - 自动检测并启动 `chaos_server.py`。
   - 注册混沌测试专用的 Model 路由（指向混沌端口）及专用 API Key。
2. **用例编排与严格断言**：
   - 用例按 `C-01` 至 `C-07` 编排。
   - **客户端主动取消验证 (`test_client_abort`)**：
     - 创建底层 Python `socket`，发送 HTTP POST 请求体；
     - 读取头部与首个 `data: ` chunk；
     - 立即调用 `sock.close()`；
     - 睡眠 0.2 秒后检查网关 `/health` 与后续标准请求转发，确保网关未被孤儿上游阻塞且正常对外服务。
3. **断言与日志捕获**：
   - 检验返回的 HTTP 状态码（502、504、413 等）。
   - 检验响应体中包含标准的 OpenAI 规范错误结构：`{"error": {"type": ..., "message": ...}}`。

### 4.3 ASan/UBSan 一键自动化验证脚本 (`scripts/run_chaos_asan.sh`)

1. **编译配置**：
   - 构建目标目录：`.build-asan`。
   - CMake 命令：
     ```bash
     cmake -B .build-asan -DAIGATE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
     cmake --build .build-asan --target aigate unit_tests -j$(nproc)
     ```
2. **运行期环境变量**：
   ```bash
   export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:detect_stack_use_after_return=1:halt_on_error=1"
   export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
   ```
3. **执行流程**：
   - 第一阶段：执行 C 语言基础单元测试 (`ctest --test-dir .build-asan --output-on-failure`)，保证基础数据结构、LRU、哈希表在 ASan 下安全；
   - 第二阶段：拉起基于 ASan 编译的 `aigate` 实例和 `chaos_server.py`；
   - 第三阶段：运行 `tests/chaos/test_chaos.py` 执行全量故障注入与客户端取消测试；
   - 第四阶段：向 `aigate` 发送 `SIGTERM` 信号使其优雅退出；
   - 第五阶段：检查 `aigate` 进程退出状态码与 ASan 报告，若存在内存泄漏或报错，控制台输出红色错误并以非 0 退出，阻断 CI 流程。

---

## 5. 错误映射与协议规范

在各种故障场景下，网关需统一按以下规范向调用方返回错误结构：

```json
{
  "error": {
    "message": "Upstream service error or connection aborted",
    "type": "upstream_error",
    "code": "bad_gateway"
  }
}
```

- **上游超时 (C-01)**：HTTP 504，`type: timeout_error`，`code: gateway_timeout`。
- **连接意外中断 / 畸形上游数据 (C-03, C-05, C-06)**：HTTP 502，`type: upstream_error`，`code: bad_gateway`。
- **超限报文 (C-07)**：HTTP 413，`type: invalid_request_error`，`code: payload_too_large`。

---

## 6. 验证与验收准则 (Acceptance Criteria)

1. **用例执行率**：`tests/chaos/test_chaos.py` 中的全部混沌测试场景用例执行成功率达到 **100%**。
2. **ASan / UBSan 洁净度**：
   - 全程无 Heap-buffer-overflow、Stack-buffer-overflow 或 Global-buffer-overflow。
   - 全程无 Use-after-free 或 Double-free。
   - 进程退出时，LeakSanitizer 报告 `0 byte(s) leaked in 0 allocation(s)`。
3. **无孤儿进程与端口占用**：测试中断或结束时，所有后台服务（`aigate` 与 `chaos_server.py`）均能安全平稳退出，无残留。
