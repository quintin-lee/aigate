# aigate Helm Chart

用于在 Kubernetes 上部署高可用、企业级 **aigate** AI API 网关的官方 Helm Chart。

## 特性

- 🔒 **严格安全加固**：只读根文件系统、非 Root 运行（UID 10001）、剥离全部 Capabilities、Fail-Fast 敏感密钥拦截校验。
- ⚡ **长连接与流式优化**：针对 SSE / 流式输出（Chat Streaming）预置 Ingress 缓冲禁用配置。
- 📈 **全方位可观测性**：开箱即用 Prometheus Pod 标注与可选的 Prometheus Operator `ServiceMonitor`。
- 🛡️ **高可用发布保障**：内置 PodDisruptionBudget（PDB）与 HorizontalPodAutoscaler（HPA）。

## 前置依赖

- Kubernetes 1.22+
- Helm 3.8.0+
- 可达的 PostgreSQL 实例（14+）
- 可达的 Redis 实例（6+，推荐）

## 快速安装

```bash
# 添加并安装（敏感项必填）
helm install aigate deploy/helm/aigate \
  -n aigate --create-namespace \
  --set secrets.pgDsn="postgresql://user:password@postgres:5432/aigate?sslmode=disable" \
  --set secrets.adminToken="your-secure-admin-token"
```

## 配置参数表

下表列出 `values.yaml` 中常用配置项及默认值：

| 参数 | 说明 | 默认值 |
| :--- | :--- | :--- |
| `replicaCount` | 初始 Pod 副本数 | `2` |
| `image.repository` | 容器镜像仓库 | `aigate` |
| `image.tag` | 容器镜像标签（为空时取 `Chart.AppVersion`） | `""` |
| `image.pullPolicy` | 镜像拉取策略 | `IfNotPresent` |
| `config.listen` | 网关监听端口 | `":8080"` |
| `config.workerThreads` | 网关内部工作线程数 | `64` |
| `config.requestTimeoutMs` | 客户端请求超时时间（毫秒） | `300000`（5分钟） |
| `config.upstreamTimeoutMs` | 上游 Provider 请求超时（毫秒） | `60000`（1分钟） |
| `config.metricsAcl` | `/metrics` 访问控制白名单 CIDR | `"127.0.0.1,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"` |
| `config.trustedProxies` | X-Forwarded-For 信任反代 CIDR | `"10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"` |
| `config.drainTimeoutS` | 停机等待连接排空时间（秒） | `15` |
| `config.redisFailOpen` | Redis 故障时策略（1 降级放行，0 严格阻断） | `1` |
| `config.logFormat` | 日志格式（`json` 或 `text`） | `"json"` |
| `config.logLevel` | 日志级别（`debug` / `info` / `warn` / `error`） | `"info"` |
| **`secrets.existingSecret`** | 外部已有 Secret 名称（指定后跳过内部生成） | `""` |
| **`secrets.pgDsn`** | **PostgreSQL 连接串（必填，除非指定 existingSecret）** | `""` |
| **`secrets.adminToken`** | **Admin API Bearer Token（必填，除非指定 existingSecret）** | `""` |
| `secrets.masterKey` | 64 位 Hex AES-256 主密钥（Provider 密钥加密用） | `""` |
| `secrets.redisUrl` | Redis 连接串 | `""` |
| `ingress.enabled` | 是否创建 Ingress 资源 | `false` |
| `ingress.className` | Ingress 控制器类别 | `"nginx"` |
| `tls.enabled` | 是否启用 Pod 内原生 TLS 终止 | `false` |
| `tls.secretName` | TLS 证书 Secret 名称（包含 tls.crt 和 tls.key） | `""` |
| `serviceMonitor.enabled` | 是否创建 Prometheus Operator ServiceMonitor | `false` |
| `autoscaling.enabled` | 是否开启 HPA 自动弹性伸缩 | `true` |
| `autoscaling.minReplicas` | HPA 最小副本数 | `2` |
| `autoscaling.maxReplicas` | HPA 最大副本数 | `10` |
| `podDisruptionBudget.enabled` | 是否启用 PDB 干扰预算 | `true` |
| `podDisruptionBudget.minAvailable` | PDB 最小可用副本数 | `1` |

## 进阶示例

### 1. 引用外部 Secret 部署

```yaml
# custom-values.yaml
secrets:
  existingSecret: "production-aigate-secrets"
```

```bash
helm install aigate deploy/helm/aigate -f custom-values.yaml -n aigate
```

### 2. 启用 Ingress 与流式优化

```yaml
ingress:
  enabled: true
  className: "nginx"
  hosts:
    - host: "gateway.ai.example.com"
      paths:
        - path: /
          pathType: Prefix
  tls:
    - secretName: gateway-tls-cert
      hosts:
        - "gateway.ai.example.com"
```

### 3. 配置 Prometheus 指标采集

```yaml
serviceMonitor:
  enabled: true
  interval: "15s"
  labels:
    release: prometheus-stack
```

## 运维与卸载

```bash
# 升级配置
helm upgrade aigate deploy/helm/aigate -n aigate -f custom-values.yaml

# 查看发布历史
helm history aigate -n aigate

# 卸载释放
helm uninstall aigate -n aigate
```
