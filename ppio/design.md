# NVL72 机架内 PD 分离 + Mooncake Store 走 NVLink:方案设计

分支 `yaochen/nvlink`,从 `main`(`31dedebb`)切出。**范围只有一个 NVL72 机架内部**,
不做跨机架、不做拓扑感知。

## 1. 目标

P、D 内部的 EP all-to-all 已经走 NVLink(NCCL 日志 `via P2P/MNNVL` 已证实)。
现在要让另外两条 KV 数据路径也走 NVLink:

| 路径 | 数据 | 现状 | 目标 |
| --- | --- | --- | --- |
| PD 直传 | 本次请求的 prefill KV,P 的 VRAM → D 的 VRAM | RoCE(`UCX_TLS` 里 cuda_ipc 未开 MNNVL) | NVLink |
| P ↔ store | P 算完的 KV 写进 store;P/D lookup 命中时从 store 读 | 不存在(D 侧未接 store) | NVLink |

## 2. 现状(两份 YAML + 一份 json 核准)

```
P (TP8, 2 节点)                    D (TP1 DP16, 4 节点)
├── NixlConnector (producer)       └── NixlConnector (consumer)
└── MooncakeStoreConnector (both)      (无 store connector)
    └── standalone-store               --no-enable-prefix-caching
        global_segment_size = 0
        protocol = rdma
```

- 混合模式:PD 直传走 NIXL/UCX,前缀缓存走 Mooncake TE + Store
- store 是独立进程 `mc_store_rest_server`,vLLM 不 mount segment,只有 4GB local_buffer
- 前缀缓存**只接了一半**:P 在写,没人读

## 3. 三条路的传输判定

三条路用三套独立的传输栈,各自决定走不走 NVLink,互不影响:

| 路径 | 传输栈 | 谁决定 NVLink | 当前 |
| --- | --- | --- | --- |
| EP all-to-all | NCCL | `NCCL_MNNVL_ENABLE=1` + fabric clique | **NVLink** ✅ |
| PD 直传 | UCX(NIXL 后端) | `UCX_CUDA_IPC_ENABLE_MNNVL` | RoCE |
| P/D ↔ store | Mooncake TE | `MC_FORCE_MNNVL` + segment 内存带 fabric handle | RoCE |

NCCL 那份日志**只证明第一条**。它不是 UCX 的证据,也不是 TE 的证据。

## 4. D 从谁读:两条路,请求级别互斥

| D 读的是 | 走什么 | 导入谁的 handle |
| --- | --- | --- |
| **本次请求**的 prefill KV(还在 P 的 VRAM) | `NixlConnector` → UCX | **P 的 KV cache** |
| **历史请求**的同前缀 KV(早先写进 store) | `MooncakeStoreConnector` → TE | **store segment** |

`MultiConnector` 读路径独占(`multi_connector.py:392-406`):router 安排了 PD → 第一条;
没安排或 D 侧 lookup 命中 → 第二条。**两条都要通**,不是配置二选一。

P 的角色同样双重:写 store 时**导入** segment 的 handle;被 D 拉时**导出**自己 KV cache
的 handle。

## 5. fabric handle 是共同的物理约束

跨节点 NVLink 访问要求被访问的内存导出 `CU_MEM_HANDLE_TYPE_FABRIC`,而这要求内存是
CUDA VMM(`cuMemCreate`)分配的。`cudaMalloc` / `aligned_alloc` 的指针拿不到 handle。

三块内存都受此约束:

| 内存 | 谁分配 | 默认方式 | 要变成 |
| --- | --- | --- | --- |
| store segment(DRAM) | `mc_store_rest_server` | `aligned_alloc` | VMM `HOST_NUMA` + FABRIC(EGM) |
| P 的 KV cache(VRAM) | vLLM | PyTorch caching allocator → `cudaMalloc` | VMM |
| D 的 KV cache(VRAM) | vLLM | 同上 | VMM |

两个传输栈对**非 fabric 内存的处理不同**,这决定了排障难度:

| | UCX cuda_ipc | Mooncake TE nvlink |
| --- | --- | --- |
| 检测 | `does not have fabric property` 告警 | `cuMemRetainAllocationHandle` 失败 |
| 后续 | **回落到 rc** | 打 WARNING 后 **`return 0`**(`nvlink_transport.cpp:999-1002`) |
| 结果 | 能工作,但走了 RoCE | segment 能 mount、对 peer **静默不可达**,直到远端第一次读 miss |

第二列是 Mooncake 侧必须改的根因。

## 6. main 上 Mooncake 缺什么

| 有 | 没有 |
| --- | --- |
| `nvlink_intra` 协议的 fabric 分配(`client_buffer_allocation.cpp:40`,**同机**) | 跨机 MNNVL 的 host DRAM fabric 分配 |
| nvlink transport 安装(`MC_FORCE_MNNVL`,`transfer_engine_impl.cpp:381`) | segment 带 fabric handle 的任何路径 |
| — | 注册失败时的硬错误 |

所以 main 上配 `protocol=rdma` + `MC_FORCE_MNNVL`,store segment 走 `aligned_alloc`,
注册时静默变空,P 的 Put 看着成功、D 的 Get 全 miss。**不改 Mooncake,store 这条路
走不了 NVLink。**

## 7. Mooncake 侧改动设计

三项,只做 fabric 这一半,不做机架感知。

### 7.1 host DRAM 的 VMM 分配器

```
cuMemCreate(size, {location=HOST_NUMA, id=cudaDevAttrHostNumaId, handleTypes=FABRIC})
cuMemAddressReserve → cuMemMap → cuMemSetAccess(每个 device + host NUMA)
```

- access descriptor 是 `device_count + 1`:CPU 写(Store memcpy)、GPU 读,两种 location 都要
- 对齐取 `max(granularity, Slab::kSize)`,`MountSegment` 拒绝未对齐基址
- 释放时先 `cuMemRetainAllocationHandle` 再 unmap(handle 通过映射解析)
- **失败不回落**:返回 `nullptr`。回落到 `aligned_alloc` 只是把失败推迟到远端读 miss
- 驱动查不到 `cudaDevAttrHostNumaId` → 失败,**不猜 node 0**

### 7.2 分配谓词:不能只看 protocol 字符串

nvlink transport 装不装,由编译开关 + `MC_FORCE_MNNVL` / `MC_INTRANODE_NVLINK` / 无 HCA
决定,`transfer_engine_->init()` 不接收 protocol。而 `real_client.cpp` 全文无 `"nvlink"`
字面量,`IsHostStoreSegmentProtocol()` 白名单也没有它 —— 写 `protocol=nvlink` 会丢
`cudaHostRegister`。

所以谓词问 TE 实际装了什么:`TransferEngine::nvlinkUsesFabricMem()` → 已有的
`MultiTransport::nvlinkUsesFabricMem()`。setup 时查一次,存进进程级标志,分配和释放读
同一个值(否则 `free()` 一个 VMM 映射会 abort)。protocol 照常写 `rdma`。

### 7.3 静默失败改硬错

`nvlink_transport.cpp:1002` 的 `return 0` 改成返回错误。不是功能,是让配置错误在
启动时暴露,而不是在生产流量的第一次 miss 时。

### 7.4 env 开关语义

`MC_STORE_HOST_FABRIC` 取值判定(`=0` 是关,无效值告警并保持关)。与 `MC_STORE_USE_HUGEPAGE`
的存在即生效相反 —— 静默 opt-in 选中的失效模式正是 §5 那条。

## 8. NIXL / UCX:零改造

nixl 1.4.1 自带的 UCX(`nixl_cu13.libs/ucx/libuct_cuda.so`)二进制内已核准:

```
ENABLE_MNNVL                              ← 配置项
nvmlDeviceGetGpuFabricInfoV               ← 与 NCCL 同一个 clique 判定
cuMemPoolExportToShareableHandle(... FABRIC ...)  ← 能接 mempool 导出
different machine and no MNNVL            ← 当前命中的回落分支
```

只需 `UCX_CUDA_IPC_ENABLE_MNNVL=y`,P、D 两侧。`UCX_TLS` 里 `cuda_ipc` 已在,不动。

## 9. vLLM KV cache 带 fabric handle:vLLM 自带,一个参数

两条路都要它(§5)。vLLM v0.29.0 **已内置**,不需要任何外部组件:

```
--enable-cumem-allocator
  → gpu_worker.py:734      KV cache 分配包在 use_memory_pool(tag="kv_cache")
  → cumem.py:77-78         torch.cuda.memory.use_mem_pool(MemPool(CUDAPluggableAllocator))
  → cumem_allocator.cpp:143-148   FABRIC_SUPPORTED → requestedHandleTypes = FABRIC
  → cuMemCreate(DEVICE, FABRIC)
```

否掉的两条:

| 方案 | 为什么不行 |
| --- | --- |
| `expandable_segments:True` | vLLM 主动拒绝(`config/vllm.py:1000-1041`)。VMM 重映射让 pin 住的注册指向过期页,第一次跨节点传输 `IBV_WC_REM_ACCESS_ERR`。这是正确的拒绝 |
| 参考部署的 `kv_fabric_alloc.so` + patch | 与 cumem **同一套机制**(pluggable allocator + MemPool)。上游已有,不该自己维护一份带版本绑定锚点的 |

硬件侧已核准(host-10-0-3-71,GB300):`cuMemCreate(DEVICE, FABRIC)` 与
`cuMemExportToShareableHandle` 均 rc=0;`nvidia-smi -q` 四卡 Fabric `State: Completed`,
`CliqueId 32766`,ClusterUUID 一致。

**唯一要盯的**:cumem 在 FABRIC 失败时静默回落 POSIX FD(`:157-160`),分配不报错。
所以要靠 UCX 日志 `does not have fabric property` 反向确认没回落。

## 10. 部署形态

```
mc_store_rest_server (独立 pod)        ← MC_STORE_HOST_FABRIC=1, MC_FORCE_MNNVL=1, protocol=rdma
        ▲ Put (TE nvlink)      ▲ Get (TE nvlink)
        │                      │
P (TP8) ──── NixlConnector ────▶ D (TP1×16)
  MultiConnector:                MultiConnector:          ← D 要加 store connector
    Nixl(producer)                 Nixl(consumer)
    Store(kv_both)                 Store(consumer, lookup) ← D 要开 prefix caching
```

env 三组,谁的进程设谁的:

| 进程 | env |
| --- | --- |
| store | `MC_STORE_HOST_FABRIC=1` `MC_FORCE_MNNVL=1` json `protocol=rdma`;非 privileged 时加 `NVIDIA_IMEX_CHANNELS=0` |
| P / D | `UCX_CUDA_IPC_ENABLE_MNNVL=y` `MC_FORCE_MNNVL=1` `MOONCAKE_CONFIG_PATH`;CLI 加 `--enable-cumem-allocator` |

`MC_FORCE_MNNVL` 的代价(无 RDMA 回落)在机架内**不成立** —— 没有跨 domain 流量。

## 11. 验证:事实依据,不靠配置

每条路各自一套证据,缺一不可:

| 路径 | 日志 | 计数器 |
| --- | --- | --- |
| PD 直传 | `UCX_LOG_LEVEL=info` → `fabric_info: state=3 uuid=...`,且**无** `does not have fabric property` | PD 交接期间 `mlx5_bond_*/ports/1/hw_counters/rx_write_requests` **不涨**,`nvidia-smi nvlink -gt d` Rx 涨 |
| P/D ↔ store | store 进程 `Using NVLink transport` + `Allocated ... fabric host memory`;P/D 进程 `Using NVLink transport` | store 读写期间同上 |
| 静默失效排除 | D 侧 store lookup 命中后**无** miss 日志 | — |

用 `hw_counters/rx_write_requests` 而非 `port_rcv_data`:前者只对 RDMA verb 计数,不被
TCP 流量污染。EP 流量会干扰 NVLink 计数器,差分要在 decode 稳定期(EP 在跑、KV 不动)
和交接期(两者都动)分开做。

## 12. 边界

| 不做 | 原因 |
| --- | --- |
| 跨机架 / 拓扑感知 | 范围外。单 NVL72 内无 domain 边界 |
| `MooncakeConnector` 替换 `NixlConnector` | NIXL 直传只差一个 env,没有换的理由 |
| VRAM fabric 分配(`MC_STORE_VRAM_FABRIC`) | store 是 DRAM 池,不需要 |
| D 写回 decode KV(`save_decode_cache`) | 可选,先不做,通了再加 |
