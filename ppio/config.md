# 配置：4 × GB300 NVL72

72 台机器（4 机架 × 18 台），每台同时可以是 prefill / decode / store server。
本文给出每个组件的**完整可抄配置**。

## 0. 机架划分

**`rack_id` 必须与 NVLink 域（IMEX domain）边界完全一致。**
一个 NVL72 机架 = 一个 NVLink 域 = 一个 `rack_id`。

不要按物理机柜位置划分，也不要让一个 `rack_id` 跨越两个 NVLink 域——
那会让 master 以为两组节点间有 NVLink，实际没有。

| 机架 | `rack_id` | 节点 |
| --- | --- | --- |
| NVL72 #0 | `rack0` | node-r0-01 … node-r0-18 |
| NVL72 #1 | `rack1` | node-r1-01 … node-r1-18 |
| NVL72 #2 | `rack2` | node-r2-01 … node-r2-18 |
| NVL72 #3 | `rack3` | node-r3-01 … node-r3-18 |

同机架 18 台的 `rack_id` 必须**完全相同**，4 个机架互不相同。

## 1. 配置优先级

```
配置文件（MOONCAKE_CONFIG_PATH 或 --config）  >  环境变量
```

**两套不要混用。** 同时存在时环境变量**不生效**——`load_from_env()` 只在
没有配置文件时才走。这是最容易踩的坑：设了 `MOONCAKE_RACK_ID` 却又指了配置文件，
机架感知静默失效。

`rack_id` 必须在 mount segment **之前**设好。`SetRackAffinity()` 在
`setup_internal` 中于建 client 之后、mount 之前调用。延后设置会让 segment
带不上机架身份，master 的 `RackSegmentIndex` 里就没有这个节点，整套机制静默失效。

## 2. master

**master 不需要任何机架配置。** 它从各 segment 上报的 `rack_id` 自动建索引。

```bash
mooncake_master \
  --port=50051 \
  --metrics_port=9003 \
  --v=1
```

`--v=1` 必须开，否则看不到放置决策日志（见第 6 节）。

| flag | 默认 | NVL72 建议 |
| --- | --- | --- |
| `--port` | 50051 | 保持 |
| `--metrics_port` | 9003 | 保持 |
| `--rpc_address` | 0.0.0.0 | 保持 |
| `--enable_ha` | false | 72 节点规模建议开，需 etcd/redis 后端 |
| `--eviction_high_watermark_ratio` | — | strict 模式下要按**单机架**容量核算，见第 3 节警告 |
| `--v` | 0 | **设 1**，否则无放置日志 |

HA 部署加 `--enable_ha=true`，并按 `STORE_USE_ETCD` / `STORE_USE_REDIS` 编译选项
准备对应后端。

## 3. store server（数据来源，必配）

store server 就是一个贡献 `global_segment` 的 client 进程。
**它的 `rack_id` 是整套机制的数据来源，省掉机架感知就完全不工作。**

`/etc/mooncake/store.json`（18 台一份，只改 `local_hostname`）：

```jsonc
{
  "local_hostname": "node-r0-03",
  "metadata_server": "P2PHANDSHAKE",
  "master_server_address": "10.0.0.1:50051",
  "protocol": "rdma",
  "device_name": "mlx5_0",
  "global_segment_size": "64gb",
  "local_buffer_size": "2gb",
  "rack_id": "rack0",
  "enable_client_http_server": true,
  "client_http_port": 9300
  // store server 不是写入方，不需要 strict_rack
}
```

启动：

```bash
python3 -m mooncake.mooncake_store_service \
  --config /etc/mooncake/store.json \
  --port 8080 \
  -Dlocal_hostname=node-r0-03 \
  -Drack_id=rack0
```

`-D` 可覆盖任意单项，适合一份配置分发到 72 台后只改机架号和主机名。

| 字段 | NVL72 取值 | 说明 |
| --- | --- | --- |
| `rack_id` | `rack0`…`rack3` | **必填**，按所在机架 |
| `protocol` | `rdma` | 见第 5 节：`nvlink` 当前不可用 |
| `device_name` | `mlx5_0` 等 | ConnectX-8。多网卡可用 `auto-discovery` |
| `global_segment_size` | 按每机可贡献的 DRAM | 支持 `64gb` 这类后缀 |
| `local_buffer_size` | `2gb` 起 | store server 自身读写用 |
| `strict_rack` | **不设** | store server 不是写入方 |

⚠️ **strict 模式下的容量水位**：写入锁在本机架内，单机架写满就失败、不跨机架兜底。
需要按**机架**而不是全集群监控容量与淘汰率。

## 4. client：prefill 与 decode

两者用同一个 wheel，**区别只在 `strict_rack`**。

### prefill（写入侧：硬约束）

```bash
export MOONCAKE_RACK_ID=rack0        # 按所在机架
export MOONCAKE_STRICT_RACK=true
```

语义：master **只**在 rack0 的 store server 上分配副本。rack0 容量不足时
触发淘汰并返回 `NO_AVAILABLE_HANDLE`，**不会**写到 rack1–3。

⚠️ **副本数约束**：`strict_rack=true` 时 `replica_num` 不能超过**本机架**可服务的
segment 数（这里是 18），否则分配必然失败。

`strict_rack=true` 但 `rack_id` 为空时会打 WARNING 并退回非 strict，
不会静默改变放置行为。

### decode（读取侧：软偏好）

```bash
export MOONCAKE_RACK_ID=rack0        # 按所在机架
export MOONCAKE_STRICT_RACK=false    # 或干脆不设
```

语义：优先挑同机架副本；同机架没有就读其他机架（走 RDMA），不会失败。

**decode 侧不要开 strict。** strict 只影响写入放置，在 decode 上开它没有收益，
而且当 decode 自身也做写入时（例如 chunked prefill 回填）会意外限制放置。

### 读取优先级（实际行为）

```
local MEMORY > local NOF_SSD > 同机架 MEMORY > 远端 MEMORY > 远端 NOF_SSD > LOCAL_DISK > DFS > DISK
```

两点容易误判：

1. **本地 NOF_SSD 仍优先于同机架 MEMORY。** 沿用机架感知之前的既有行为，本次没改。
   GB300 上这个顺序其实值得重新考虑（同机架 NVLink DRAM 约 900 GB/s 量级
   vs 本地 NVMe 约 10 GB/s 量级），但那是独立决策，**目前未做**。
2. **没有 `rack_id` 的副本被当成远端。** 所以未配置机架的节点行为完全不变，
   机架感知是纯 opt-in；`rack_id` 为空时逐字节等价于历史行为。

### 客户端完整环境变量

| 变量 | store server | prefill | decode |
| --- | --- | --- | --- |
| `MOONCAKE_RACK_ID` | 必须 | 必须 | 必须 |
| `MOONCAKE_STRICT_RACK` | 不设 | `true` | `false` / 不设 |
| `MOONCAKE_MASTER` | 必须 | 必须 | 必须 |
| `MOONCAKE_TE_META_DATA_SERVER` | `P2PHANDSHAKE` | 同 | 同 |
| `MOONCAKE_PROTOCOL` | `rdma` | 同 | 同 |
| `MOONCAKE_DEVICE` | `mlx5_0` | 同 | 同 |
| `MOONCAKE_GLOBAL_SEGMENT_SIZE` | 按机器 | **0 或不设** | **0 或不设** |
| `MOONCAKE_LOCAL_BUFFER_SIZE` | `2gb` | 按并发 | 按并发 |
| `MOONCAKE_LOCAL_HOSTNAME` | 本机 | 本机 | 本机 |

`strict_rack` 的布尔解析接受 `true/false/1/0/yes/no`（大小写不敏感）。

## 5. fabric handle 开关（谨慎）

```bash
export MC_STORE_VRAM_FABRIC=1
```

生效需要**三个条件同时满足**，缺任一都降级为 `cudaMalloc`：

1. 编译期 `USE_VRAM_SEGMENT=ON` **且** `USE_MNNVL=ON`（未开 HIP/MUSA/UBSHMEM）
2. 运行时 `protocol == "nvlink"`
3. `MC_STORE_VRAM_FABRIC` 解析为真

取值语义（**按值判断**，不是「存在即生效」）：

| 取值 | 结果 |
| --- | --- |
| `1` / `true` / `yes` / `on` / `enable`（大小写不敏感，容忍首尾空格） | 开启 |
| `0` / `false` / `no` / `off` / `disable` | 关闭 |
| 空字符串、`maybe` 等无法解析的值 | 关闭 + WARNING 回显原值 |
| 未设置 | 关闭 |

与 `MC_STORE_USE_HUGEPAGE` 语义**不同**：后者「存在即生效」，设 `0` 也算开；
前者设 `0` 就是关。这样设计是因为，如果 `=0` 被当成开启，它选中的失败模式恰好是
「注册成功但什么都没注册」的静默故障，要等到远端读不到数据才暴露。

### 绝对不要设的四个变量

| 变量 | 后果 |
| --- | --- |
| `MC_STORE_USE_HUGEPAGE` | **存在即生效**（设 `0` 也算开）。分配在到达 VRAM 路径前被 hugepage 截走，fabric 开关等于没开 |
| `MC_USE_NVLINK_IPC` | `supportFabricMem()` 直接 `return false`（`nvlink_transport/nvlink_transport.cpp:570`），静默回落 `cudaMalloc` |
| `MC_FORCE_MNNVL` | 见下面「死结」，**会让该节点彻底没有 RDMA** |
| `MC_INTRANODE_NVLINK` | 只装 `nvlink_intra`（单机 CUDA IPC），跨机完全不通 |

同时要避免触发 NUMA 分段（`protocol=="rdma"` 且检测到多个 NIC NUMA 节点时触发，
日志有 `NUMA-segmented mode: NIC NUMA nodes=[...]`），它同样会绕开 VRAM 路径。
分配路径优先级：

```
numa segments  >  ascend/ubshmem  >  hugepage  >  allocate_buffer_allocator_memory
                                      ↑                        ↑
                                  会在这里被截走          只有这条能到 VRAM
```

### `MC_FORCE_MNNVL` 的死结

`transfer_engine_impl.cpp:411-455` 的 transport 安装是**互斥 if/else**：

```
MC_INTRANODE_NVLINK 设了       → 只装 nvlink_intra
否则 force_mnnvl 或 (无 HCA)   → 只装 nvlink       ← 没有 rdma
否则                           → 只装 rdma
```

GB300 带 ConnectX-8，一定检测到 HCA，所以默认只装 `rdma`。
而 `docs/source/getting_started/supported-protocols.md` 说「`protocol=rdma` 且有 HCA
时要用 MNNVL 必须设 `MC_FORCE_MNNVL=true`」——**一设就只有 nvlink，
一个 rdma transport 都没有**。于是：

- 不设 → NVLink 压根没进数据路径，fabric handle 白导出
- 设了 → 该节点跨机架读**直接失败**，没有任何回落路径

**结论：NVLink 进跨机架数据路径，在「双 transport 并存」落地前没有可用配置。**
`MC_STORE_VRAM_FABRIC=1` 现在唯一的用途，是在一个**不需要跨机架通信的隔离节点**上
验证分配侧是否正确。所以 NVL72 生产配置里 `protocol` 一律填 `rdma`。

## 6. 上线验证

### 机架感知是否生效

master 开 `--v=1`，看放置决策日志：

```
# strict 模式
key=..., rack_id=rack0, strict_rack_excluded=54, rack_serving_segments=18
# soft 模式
key=..., rack_id=rack0, rack_preferred_segments=18
```

`strict_rack_excluded=54` 对应 72 节点里排除的 54 个非本机架 segment，
`rack_serving_segments=18` 对应本机架的 18 个。**这两个数字对不上，
说明 `rack_id` 划分与实际拓扑不一致。**

逐项自检：

- 每个 store server 都配了 `rack_id`，同机架 18 台完全相同
- 4 个 `rack_id` 互不相同，边界与 NVLink 域一致
- prefill 的 `replica_num` ≤ 18
- 没有同时用配置文件和环境变量

### fabric 是否生效（仅隔离节点验证时）

**成功**：

```
VRAM Segment allocated <N> bytes as an exportable fabric allocation, base=0x..., alignment=16777216
```

**失败**（来自 `NvlinkTransport::registerLocalMemory`）：

```
Memory region 0x... is not allocated by cuMemCreate, but it can be used as local buffer
```

⚠️ 这条 WARNING 是**整套机制最危险的地方**：出现它时 `registerLocalMemory`
会 `return 0`，注册「成功」但实际什么都没注册。mount 不报错、启动看起来正常，
但这块 buffer 根本没进 fabric，远端永远拿不到数据——**只能靠这条日志发现**。
部署后务必 grep 一次。

### 真机（NVL72）必须确认

- `CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED` 在**全部** GPU 上为真，
  否则 `supportFabricMem()` 返回 false，静默回落 `cudaMalloc`
- IMEX domain 覆盖整个 NVL72 的 18 个节点，且与 `rack_id` 划分一致
- **未设** `MC_USE_NVLINK_IPC`
- `cuMemGetAllocationGranularity` 的实际取值。代码按 `max(granularity, 16MB)`
  处理对齐（cachelib 的 `Slab::kSize` 要求），granularity 远大于 16 MB 时
  实际映射量会超过配置的 `global_segment_size`，需留余量
- 容器部署时 IMEX channel 设备节点已透传（见 `build.md`）

## 7. 端口

| 服务 | 默认 | flag / 字段 |
| --- | --- | --- |
| master RPC | 50051 | `--port` |
| master metrics | 9003 | `--metrics_port` |
| store service REST | 8080 | `--port` |
| client HTTP | 9300 | `client_http_port` |

## 8. 常见问题

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 放置完全没有机架偏好 | store server 没配 `rack_id` | master 索引是空的，见第 3 节 |
| 环境变量不生效 | 同时存在配置文件 | 配置文件优先，见第 1 节 |
| 写入报 `NO_AVAILABLE_HANDLE` | strict 下本机架容量不足，或 `replica_num` > 18 | 扩容、降 `replica_num`、或关 strict |
| `MC_STORE_VRAM_FABRIC=1` 没反应 | 编译缺开关，或 `protocol != nvlink` | 看 WARNING，见第 5 节 |
| 日志有 `is not allocated by cuMemCreate` | 内存不是 fabric 分配的，注册了个空 | 见第 6 节，静默故障 |
| 日志有 `NUMA-segmented mode` | 触发 NUMA 分段，到不了 VRAM 路径 | 见第 5 节 |
| 跨机架读取失败且不回落 | 设了 `MC_FORCE_MNNVL` | 取消它，见第 5 节死结 |

设计背景见 `20260918.md`，构建见 `build.md`，总览见 `index.html`。
