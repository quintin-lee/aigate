# aigate 混沌故障注入与 ASan 内存安全测试套件

本套件用于模拟高并发与不可靠网络下的上游恶劣异常场景（慢速滴灌、突发 TCP 掉线、客户端提前取消、畸形损坏数据），并在 AddressSanitizer (ASan) 与 UndefinedBehaviorSanitizer (UBSan) 环境下全面验证网关的工程健壮性与内存安全性。

---

## 1. 场景矩阵 (Chaos Scenarios)

| 场景代号 | 场景名称 | 注入行为 | 预期保护与验证点 |
|---|---|---|---|
| **C-01** | `slow-header` | 上游慢速按字节滴灌 Response Header (Slowloris) | 网关上游超时机制切断连接，释放上下文句柄 |
| **C-02** | `slow-stream` | 上游在发送 SSE 流时，每个分块人为间隔 0.3s | 流式转发管道保持平稳，Worker 线程正常流动 |
| **C-03** | `upstream-drop` | 上游在输出 2 个 chunk 后强行中断 Socket (TCP RST/FIN) | 网关识别上游异常断开，平稳收尾，无野指针/内存越界 |
| **C-04** | `client-abort` | 客户端收到首个 token chunk 后立即强行关闭连接 | 网关感知客户端断开，及时中止上游 libcurl，无孤儿请求 |
| **C-05** | `bad-chunked` | 上游发送非法的 HTTP Chunked 分块长度与损坏字节 | 传输层解析防护生效，返回 HTTP 502，防缓冲区溢出 |
| **C-06** | `truncated-json` | 上游 Content-Length 与实际不符并在 JSON 语法中间截断 | 协议解析层优雅捕获错误，返回 HTTP 502，防解析崩溃 |
| **C-07** | `oversized-payload` | 客户端发送超大请求体冲击网关 (2MB+) | 网关流控与尺寸防护拦截或安全丢弃，防 OOM 内存溢出 |

---

## 2. 快速使用

### 2.1 常规快速验证 (Fast Mode)

直接使用当前二进制运行全套混沌测试（耗时 ~3-5 秒）：

```bash
python3 tests/chaos/test_chaos.py
```

指定端口或外部网关实例：
```bash
python3 tests/chaos/test_chaos.py --gateway-port 18088 --chaos-port 19098
```

---

### 2.2 完整 ASan/UBSan 沙箱构建与内存泄漏检测 (Full Sanitizer Mode)

一键自动创建 `.build-asan` 沙箱，开启 `-DAIGATE_SANITIZERS=ON`，执行单元测试与全量混沌用例，并在网关进程退出时验证 **零内存泄漏、零未定义行为**：

```bash
./scripts/run_chaos_asan.sh
```
