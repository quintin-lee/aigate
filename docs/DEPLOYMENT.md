# aigate 生产与集群部署指南

本文档提供 **aigate** 网关的生产环境部署、集群编排与运维调优指南。网关支持以下四种部署形态：

1. [Docker Compose 单机多节点部署（开发/测试）](#一docker-compose-部署开发测试)
2. [Kubernetes Helm 部署（生产推荐）](#二kubernetes-helm-部署生产推荐)
3. [本地 Minikube 开发与验证](#三本地-minikube-开发与验证)
4. [Kubernetes 原生 Manifests 部署](#四kubernetes-原生-manifests-部署)
5. [监控告警与可观测性集成](#五监控告警与可观测性集成)
6. [生产高可用与安全基线加固](#六生产高可用与安全基线加固)

---

## 架构与依赖前置要求

- **数据库**：PostgreSQL 14+（网关启动时自动运行幂等增量迁移）。
- **分布式缓存/锁**：Redis 6+（用于分布式 QPS 限流、集群熔断状态同步与并发控制）。
- **计算资源推荐**：
  - 生产单 Pod 最低配置：`CPU 500m, Memory 512Mi`
  - 推荐单 Pod 限制：`CPU 2000m, Memory 2Gi`
  - 压测与容量规划详情可参考 [deploy/bench/SIZING.md](../deploy/bench/SIZING.md)。

---

## 一、Docker Compose 部署（开发/测试）

适用于开发联调、CI 自动化测试与单机双节点高可用验证。

### 1. 配置环境变量

从模板生成 `.env` 文件：

```bash
cp .env.example .env
```

核心变量说明（必填）：
```ini
# PostgreSQL 密码
POSTGRES_PASSWORD=your_strong_postgres_password

# 网关 Admin API 鉴权密钥（Bearer Token）
AIGATE_ADMIN_TOKEN=your_strong_admin_token

# 主密钥（64 位 Hex 字符串，AES-256-GCM 加密各 Provider 密钥用）
AIGATE_MASTER_KEY=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef

# 网关主机暴露端口（默认 8080）
AIGATE_LISTEN_PORT=8080
```

### 2. 启动集群

```bash
docker compose up -d --build
```

该命令会启动 4 个容器：
- `postgres`：主数据库（端口 5432，数据持久化于命名卷 `pgdata`）
- `redis`：分布式缓存与限流（端口 6379）
- `aigate`：主网关节点（监听 `:8080`）
- `aigate-peer`：对等网关节点（监听 `:8081`，模拟多实例多活）

### 3. 健康检查

```bash
# 检查容器状态（STATUS 显示 Up (healthy)）
docker compose ps

# 探测 HTTP 探针
curl -i http://localhost:8080/live
curl -i http://localhost:8080/ready
```

---

## 二、Kubernetes Helm 部署（生产推荐）

官方 Helm Chart 位于 [`deploy/helm/aigate`](../deploy/helm/aigate)，内置生产级安全加固、HPA 弹性扩缩容、PDB 干扰预算及 Prometheus 指标采集配置。

### 1. 快速安装

```bash
# 1. 创建生产命名空间
kubectl create namespace aigate

# 2. 通过 Helm 安装
helm install aigate deploy/helm/aigate \
  -n aigate \
  --set secrets.pgDsn="postgresql://aigate:password@postgres-service:5432/aigate?sslmode=disable" \
  --set secrets.adminToken="your-production-admin-token" \
  --set secrets.masterKey="64_hex_aes_key" \
  --set secrets.redisUrl="redis://:redis-password@redis-service:6379/0"
```

> ⚠️ **安全警告**：Chart 内置 Fail-Fast 校验机制，若未提供 `secrets.pgDsn` 与 `secrets.adminToken`，`helm install/upgrade` 将自动终止并报错，防止密钥裸奔。

### 2. 挂载外部现有 Secret（推荐生产最佳实践）

若企业使用外部密钥管理服务（如 HashiCorp Vault、AWS Secrets Manager、SealedSecrets 等），建议直接引用外部 Secret：

```bash
# 预先创建 Secret
kubectl create secret generic aigate-custom-secrets -n aigate \
  --from-literal=AIGATE_PG_DSN="postgresql://..." \
  --from-literal=AIGATE_ADMIN_TOKEN="..." \
  --from-literal=AIGATE_MASTER_KEY="..." \
  --from-literal=AIGATE_REDIS_URL="..."

# Helm 引用现有 Secret（跳过内部 Secret 生成）
helm install aigate deploy/helm/aigate \
  -n aigate \
  --set secrets.existingSecret="aigate-custom-secrets"
```

### 3. 企业级特性开关

在 `values.yaml` 或通过 `--set` 启用进阶特性：

#### ① 启用 Ingress 与流式优化
```bash
--set ingress.enabled=true \
--set ingress.className="nginx" \
--set ingress.hosts[0].host="api.yourcompany.com" \
--set ingress.hosts[0].paths[0].path="/" \
--set ingress.hosts[0].paths[0].pathType="Prefix"
```
*Chart 已内置 `proxy-buffering: off` 与长连接配置，原生保障 SSE（Server-Sent Events）流式响应零缓冲。*

#### ② 启用 Pod 内原生 TLS 终止
```bash
--set tls.enabled=true \
--set tls.secretName="aigate-tls-cert"
```

#### ③ 启用 Prometheus Operator ServiceMonitor
```bash
--set serviceMonitor.enabled=true \
--set serviceMonitor.interval="15s"
```

### 4. 滚动更新与回滚

```bash
# 修改配置并平滑滚动升级（如调大工作线程）
helm upgrade aigate deploy/helm/aigate \
  -n aigate \
  --reuse-values \
  --set config.workerThreads=128

# 查看历史版本
helm history aigate -n aigate

# 一键秒级回滚
helm rollback aigate 1 -n aigate
```

---

## 三、本地 Minikube 开发与验证

若需要在本地单机快速模拟完整的 Kubernetes 环境：

```bash
# 1. 启动 Minikube
minikube start --driver=docker

# 2. 配置代理白名单（防止本地 HTTP 代理拦截集群通信）
export NO_PROXY=192.168.49.2,localhost,127.0.0.1
export no_proxy=192.168.49.2,localhost,127.0.0.1

# 3. 将本地构建的镜像载入集群
docker tag aigate-aigate:latest aigate:latest
minikube image load aigate:latest
minikube image load postgres:16-bookworm redis:7-alpine

# 4. 创建命名空间并拉起后端依赖
kubectl create namespace aigate
kubectl run postgres -n aigate --image=postgres:16-bookworm --image-pull-policy=Never \
  --env=POSTGRES_PASSWORD=changeme --env=POSTGRES_USER=aigate --env=POSTGRES_DB=aigate --port=5432 --expose
kubectl run redis -n aigate --image=redis:7-alpine --image-pull-policy=Never --port=6379 --expose

# 5. Helm 部署网关
helm install aigate deploy/helm/aigate -n aigate \
  --set image.pullPolicy=Never \
  --set secrets.pgDsn="host=postgres dbname=aigate user=aigate password=changeme" \
  --set secrets.adminToken="dev-admin-token" \
  --set secrets.redisUrl="redis://redis:6379"

# 6. 本地端口转发
kubectl port-forward --address 0.0.0.0 -n aigate svc/aigate 8080:8080
```

---

## 四、Kubernetes 原生 Manifests 部署

若不使用 Helm，可使用 [`deploy/kubernetes/`](../deploy/kubernetes) 目录下的原生 YAML 文件：

```bash
# 1. 按序应用配置文件
kubectl apply -f deploy/kubernetes/secret.yaml
kubectl apply -f deploy/kubernetes/configmap.yaml
kubectl apply -f deploy/kubernetes/service.yaml
kubectl apply -f deploy/kubernetes/deployment.yaml
kubectl apply -f deploy/kubernetes/hpa.yaml
kubectl apply -f deploy/kubernetes/pdb.yaml

# 2. 可选：应用 Ingress
kubectl apply -f deploy/kubernetes/ingress.yaml
```

---

## 五、监控告警与可观测性集成

### 1. Prometheus 指标采集

网关在 `/metrics` 端点提供标准 Prometheus 文本指标：
- **核心指标**：
  - `aigate_requests_total`：总请求数
  - `aigate_errors_total`：总 5xx 错误数
  - `aigate_tokens_total` / `aigate_tokens_cached_total`：Token 总数与缓存省流
  - `aigate_failover_total`：上游模型故障转移计数
  - `aigate_upstream_ttft_seconds`：首字延迟（TTFT）直方图
  - `aigate_upstream_inflight{endpoint="..."}`：在途并发请求 Gauge
  - `aigate_concurrency_rejected_total{model="..."}`：并发饱和熔断拒绝 Counter

### 2. Prometheus 告警规则部署

告警配置文件位于 [`deploy/prometheus/alerts.yaml`](../deploy/prometheus/alerts.yaml)，包含网关宕机、5xx 突增、P99 延迟高、首字慢、上游频发 Failover 及并发容量饱和告警规则。

将告警规则加入 Prometheus：
```bash
kubectl apply -f deploy/prometheus/alerts.yaml
```

### 3. Grafana 监控看板导入

官方预置看板模板位于 [`deploy/grafana/dashboard.json`](../deploy/grafana/dashboard.json)：
1. 登录 Grafana 控制台。
2. 点击 **Dashboards** → **Import**。
3. 上传或粘贴 `deploy/grafana/dashboard.json` 内容。
4. 选择对应的 Prometheus 数据源，即可呈现包含 QPS、延迟分布、Token 经济学、TTFT 与并发限制的完整大盘。

---

## 六、生产高可用与安全基线加固

1. **容器安全上下文（Pod Security Standards）**：
   - 强制非 Root 用户运行（UID `10001` / GID `10001`）。
   - 开启 `readOnlyRootFilesystem: true`（只读根文件系统），临时目录仅允许挂载 `emptyDir` 的 `/tmp`。
   - 彻底禁用提权（`allowPrivilegeEscalation: false`）并剥离全部 Linux Capabilities（`drop: [ALL]`）。
2. **高可用弹性配置（HPA & PDB）**：
   - 配置 `minAvailable: 1` 的 PDB，避免集群维护节点驱逐导致服务完全不可用。
   - 默认启用 HPA（CPU 70%、内存 80%），动态弹性伸缩至 2~10 副本。
3. **优雅停机与无损发布（Zero-Downtime Rolling Update）**：
   - 滚动更新策略设为 `maxSurge: 25%`, `maxUnavailable: 0`。
   - 容器生命周期挂载 `preStop: sleep 5` 钩子，配合 `AIGATE_DRAIN_TIMEOUT_S=15`，确保流量从 Kubernetes Endpoints 剔除后再终结活跃连接。
