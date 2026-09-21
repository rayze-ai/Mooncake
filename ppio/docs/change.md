# 改动清单

本分支 (`yaochen/topology-aware`) 相对 `main` 的全部改动。两个 commit：

| commit | 内容 |
| --- | --- |
| `16800deb` | `[TE][Store] Rack-aware NVLink routing + snapshot fixes` |
| `a09be114` | `[Store] Add host fabric (EGM) allocation for NVLink-accessible CPU buffers` |

改动分两条线：**机架感知选路/放置**(纯软件逻辑，无编译开关)和 **fabric handle 分配**(需 CUDA，带 env 开关)。前者可独立上线，后者依赖前者才有意义。

---

## 一、Mooncake 侧改动

### 1.1 Transfer Engine：机架感知选路

**新增 `mooncake-transfer-engine/include/multi_transport_locality.h`**

选路判定的纯函数集合，独立成头文件以便单测。核心是三态可达性：

```cpp
enum class RackReachability { Reachable, Unreachable, Unknown };
```

`Unknown`(任一侧 rack_id 未设)与 `Unreachable`(两侧 rack_id 明确不同)刻意区分开：前者表示部署没告诉我们拓扑信息，后者是对拓扑的明确陈述。调用方对两者都降级，但只有 `Unreachable` 在无回落路径时才报错。

其余函数处理选路前的字符串归一化：

| 函数 | 作用 | 为什么需要 |
| --- | --- | --- |
| `segmentHost()` | 从 segment name 剥出 host | IPv6 字面量含多个冒号，`rfind(':')` 会切坏 `2001:db8::1` |
| `hostEquals()` | 大小写无关比较 | DNS 主机名大小写无关，IPv6 hex 字面量也可能只差大小写；用 `==` 会把同一主机误判为远端，丢掉同机 IPC 快路径 |
| `trimRackId()` | 去首尾空白 | rack_id 来自 env 或 YAML，尾随空格会让同机架静默变成跨机架 |
| `rackReachabilityForNvlink()` | 三态判定 | 同主机一律 `Reachable`(不看 rack_id)，保证单机部署不受影响 |

注意 rack_id 用**精确相等**比较，不做大小写归一 —— 它是运维指定的标签而非主机名，`Rack0` 和 `rack0` 完全可能是两个机架。

**`src/multi_transport.cpp`：两处选路门控**

新增两个成员函数：

- `nvlinkUsesFabric()` — 本地 nvlink transport 导出的是 fabric handle(跨主机、限一个 NVLink domain)还是 `cudaIpcMemHandle_t`(仅同主机)。由设备能力 + `MC_USE_NVLINK_IPC` 在 install 时决定，是**节点属性**而非请求属性。
- `nvlinkReachable()` — fabric 模式下比 rack，IPC 模式下比 host(复用原有 hip/musa/shm 的同机门控)。

接入 `selectTransport()` 的三个位置：

1. **协议优先级表**新增 `nvlink → 3`(可用 `MC_DISABLE_NVLINK` 关掉)。位置在 GPU-IPC 组(4)之下、`rdma`(2)之上：nvlink 比同机 IPC 覆盖更远，但同机 IPC 完全不需要 fabric import，所以让 IPC 优先。
2. **多协议 segment 扫描**中跳过 `nvlink_reachable == false` 的 buffer。
3. **单协议 nvlink segment** 的跨机架降级：segment 只宣告 nvlink，但 fabric handle 跨 NVLink domain 不可导入，所以按 `rdma → rdma_twosided → tcp` 顺序找本地已安装的跨机架 transport；都没有才返回 `NotSupportedTransport`，错误信息里带上两侧 rack_id 和具体原因。

这个降级是**提前判定**而非失败后重试 —— domain 边界是物理的，软件跨不过去，交给 nvlink 只会在 `submitTransfer` 里变成一个不透明的 fabric import 失败。

`mp_selectTransport()` 里做了对应处理(多协议偏好路径)。

**`src/config.cpp`：rack_id 读取**

按 `MC_RACK_ID` → `MOONCAKE_RACK_ID` 顺序读，取第一个非空的。`MC_RACK_ID` 是 TE 原生命名，`MOONCAKE_RACK_ID` 作为回退是因为 Store 配置已经在读它，设了一个的部署不该被迫设两个。纯空白值视为未设并告警。

**`nvlink_transport.{h,cpp}`**

- 新增 `allocatePinnedLocalMemory(size_t length, size_t alignment)` 重载。原单参数版本只对齐到 CUDA granularity(通常 2MB)，而 cachelib 要求 `Slab::kSize`(16MB)对齐，把 buffer 交给它的调用方必须用新重载。
- 新增 `usesFabricMem()` 供 `MultiTransport` 读取，决定用 rack 门控还是 host 门控。

**新增 `tests/multi_transport_locality_test.cpp`** — 覆盖 IPv6 字面量、大小写、空白、三态判定。

### 1.2 Store 侧：机架感知放置与副本选择

**`src/master_service.cpp`：写路径两种模式**

| 模式 | 条件 | 行为 |
| --- | --- | --- |
| **strict** | `strict_rack=true` 且 `rack_id` 非空 | 把 rack 外 segment 放进 `excluded_segments`。本机架满了就分配失败(触发淘汰)，不跨机架外溢 |
| **soft** | `rack_id` 非空，`strict_rack=false` | 把同机架 segment 追加进 `preferred_segments`，不排除其他。本机架满了仍跨机架走 RDMA |

两个诊断兜底：

- `strict_rack=true` 但 `rack_id` 为空 → 告警并退回非严格模式(而不是让每个写都失败)。
- strict 模式下 rack index **完全为空** → `LOG(ERROR)` 明确点名。这种情况说明没有任何 segment 报告过 rack(配置问题或 master 恢复了 pre-rack 快照)，而它产生的 `NO_AVAILABLE_HANDLE` 和"容量耗尽"长得一模一样。

**`include/replica_selection.h`：新增同机架层级**

读路径的优先级从 4 层变 5 层：

```
local MEMORY → local NOF_SSD → 同机架 MEMORY → remote MEMORY → remote NOF_SSD → LOCAL_DISK → DFS → DISK
```

同机架层插在 local 层之后、remote MEMORY 之前：同机架副本走 NVLink，跨机架副本走 RDMA。注意 **local NOF_SSD 仍然优先于同机架 MEMORY**，与该层级引入前的行为一致。

`SelectBestReplica()` 新增的 `local_rack_id` 参数**刻意不给默认值** —— 忘记传的调用方会静默丢掉机架亲和性，编译期抓不到；要显式放弃就传 `""`。无 rack 信息的副本(旧 master)按 remote 处理，所以空 `local_rack_id` 精确复现历史行为。

**`include/types.h`：线上兼容**

`Segment::rack_id` 和 `AllocatedBuffer::Descriptor::rack_id_` 用 `struct_pack::compatible<std::string, 1>` 而非普通成员 —— 普通成员会改变 struct 的 type hash，导致新旧版本的 client 与 master 无法交换 `MountSegment` 请求。

`Segment::operator==` 手写而非 `= default`：`struct_pack::compatible` 派生自 `std::optional`，会让 defaulted `operator==` 变成歧义(从而隐式删除)，默认版本在此处能编过、在第一个调用方才炸。手写版通过 `RackId()` 比较，顺带让"未设"和"显式空"相等，这也是代码其余部分对"无机架"的定义。

**配置键** (`types.h`)：`rack_id`、`strict_rack`。

### 1.3 Store 侧：fabric handle 分配

这部分是 commit `a09be114`，也是本轮新做的。按"大改动独立成文件、原有文件只做薄调用"的要求组织。

**VRAM fabric**(`MC_STORE_VRAM_FABRIC=1`)

| 文件 | 行数 | 内容 |
| --- | --- | --- |
| `src/config/vram_fabric_config.{h,cpp}` | 65 | env 解析 |
| `tests/vram_fabric_config_test.cpp` | 111 | 6 个用例 |

**Host fabric / EGM**(`MC_STORE_HOST_FABRIC=1`，本轮新增)

| 文件 | 行数 | 内容 |
| --- | --- | --- |
| `src/config/host_fabric_config.{h,cpp}` | 65 | env 解析，对照 `VramFabricConfig` |
| `src/common/host_fabric_allocator.{h,cpp}` | 256 | CUDA VMM 分配逻辑 |
| `tests/host_fabric_config_test.cpp` | 53 | 6 个用例 |

host fabric 用 CUDA VMM 分配**宿主 DRAM** 并导出 fabric handle，即 EGM (Extended GPU Memory)。关键实现点：

- `prop.location.type = CU_MEM_LOCATION_TYPE_HOST_NUMA`，`location.id` 取当前 device 的 `cudaDevAttrHostNumaId`。驱动不暴露该属性时返回负值，此时**直接失败而不猜 node 0**。
- `prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_FABRIC`。
- access descriptor 数量是 `device_count + 1`：每个 GPU 一个 `CU_MEM_LOCATION_TYPE_DEVICE`，外加一个 `CU_MEM_LOCATION_TYPE_HOST_NUMA`。与 VRAM 不同 —— 这块内存由 CPU 写入、GPU 读取，两种 location 都需要授权。
- 对齐取 `max(granularity, alignment)`(两者都是 2 的幂时)，否则用 granularity 并告警。Store 需要 `Slab::kSize` 对齐的基址，`MountSegment` 硬性拒绝未对齐的。
- 释放时**先 `cuMemRetainAllocationHandle` 再 unmap** —— 该调用是**通过映射**解析 handle 的，unmap 之后就拿不到了。size 用 `cuMemGetAddressRange` 回取，因此不需要全局 map + mutex 记录尺寸(这一点抄的是 `NvlinkTransport::freePinnedLocalMemory`)。

**为什么必须是 fabric handle**：`NvlinkTransport::registerLocalMemory()` 内部调 `cuMemRetainAllocationHandle()`，对 `cudaMalloc`/`aligned_alloc` 的指针会失败。失败后注册"成功"返回但实际什么都没注册，segment 能 mount、却对 peer 静默不可达 —— 直到远端第一次读 miss 才暴露。所以分配失败时**不回落到 `aligned_alloc`**：那个指针只会把失败推迟到 `MountSegment`，且错误信息更难懂。

**原有文件的改动**(`src/common/client_buffer_allocation.cpp`，共 60 行)：

```cpp
#if defined(USE_CUDA) && !defined(USE_VRAM_SEGMENT)
#define MOONCAKE_STORE_FABRIC_HOST 1
```

编译条件限定在"有 CUDA 但无 VRAM segment"：host fabric 只在末尾 `aligned_alloc` 回落路径上才可达，而 `USE_VRAM_SEGMENT` 构建永远走不到那里(那里所有 segment 都是设备内存)。

内容是一个谓词加两个分支：

```cpp
bool use_fabric_host(const std::string &protocol) {
    if (protocol != "nvlink") return false;
    static const bool enabled = HostFabricConfig::IsEnabledFromEnvironment();
    return enabled;
}
```

`static` 保证一次进程内只读一次 env，这样一次分配和它后来的释放对"谁拥有这块内存"的判断一致。`free_memory()` 里的分支必须与分配端**逐项镜像**(同样的 protocol 判断、同样的 env 开关)—— fabric buffer 是 VMM 映射，用 `free()` 会在 glibc 从未分发过的指针上 abort。

**`src/CMakeLists.txt`**：注册两个新 `.cpp`；`USE_CUDA` 分支补 `CUDA::cuda_driver`(VMM 的 `cuMemCreate` 等符号不在 `cudart` 里)。

### 1.4 环境变量与配置汇总

| 名称 | 位置 | 语义 |
| --- | --- | --- |
| `MC_RACK_ID` / `MOONCAKE_RACK_ID` | TE env | 机架标识，选路用 |
| `MOONCAKE_STRICT_RACK` | Store env | 严格机架模式 |
| `MC_STORE_VRAM_FABRIC` | Store env | VRAM fabric 分配 |
| `MC_STORE_HOST_FABRIC` | Store env | host DRAM fabric 分配(EGM) |
| `MC_DISABLE_NVLINK` | TE env | 从优先级表移除 nvlink |
| `rack_id` / `strict_rack` | Store 配置键 | 同上，YAML/JSON 形式 |

两个 fabric 开关都是**取值判定**而非存在判定：`=0` 明确是关。这与 `MC_STORE_USE_HUGEPAGE`(存在即生效)相反 —— 这里静默 opt-in 的后果特别糟，它选中的失效模式(注册了空东西的 segment)在远端读 miss 前完全不可见。

### 1.5 Python / wheel 侧

| 文件 | 改动 |
| --- | --- |
| `python/mooncake/mooncake_config.py` | 新增 `rack_id`、`strict_rack` 字段，从文件与 env 两条路径读 |
| `mooncake-wheel/mooncake/mooncake_store_service.py` | 把两个字段透传进 C++ 配置字典 |
| `mooncake-wheel/mooncake/fabric_allocator_utils.py` | 新增：`.so` 定位与 allocator backend 探测(`ctypes`) |
| `mooncake-wheel/mooncake/shared_segment.py` | 新增：一写多读的进程共享宿主内存(MLA 场景下 TP 组内 KV 只分配一份) |
| `mooncake-wheel/mooncake/async_store.py` | 新增：`MooncakeDistributedStore` 的 async 包装(`run_in_executor`) |

### 1.6 测试

| 测试 | 用例数 | 状态 |
| --- | --- | --- |
| `host_fabric_config_test` | 6 | 通过 |
| `vram_fabric_config_test` | 6 | 通过 |
| `buffer_allocator_test` | 21 | 通过 |
| `multi_transport_locality_test` | — | 新增 |
| `segment_test` / `replica_selection_test` / `serializer_test` | — | 扩充 |
| `test_mooncake_config.py` | — | 扩充 |

前三项在 `USE_CUDA=ON` 下实测通过(见 `build-deploy.md`)。后面几项本轮未逐个跑。

---

## 二、vLLM 侧改动

**Mooncake 仓库里没有 vLLM 的改动。** vLLM 侧是对**已安装的 vllm 包做运行时 patch**，脚本在参考部署里，不在本仓库：

```
/data/code/artifact/tp1dp4/yaochen/kv-fabric/
├── kv_fabric_alloc.cpp        # PyTorch pluggable allocator
└── apply_kv_fabric_patch.sh   # patch vllm 的 attn_utils.py
```

### 2.1 为什么要改 vLLM

Mooncake 侧把 Store 的 segment 变成 fabric 可导出的，但 **KV cache 本身是 vLLM 分配的**。vLLM 默认用 `torch.zeros(..., device="cuda")`，走的是 PyTorch caching allocator → `cudaMalloc`，没有 fabric handle。这样的显存无法被远端节点经 NVLink 导入，NVLink KV 通路缺了最关键的一环。

### 2.2 `kv_fabric_alloc.cpp`

一个 PyTorch pluggable allocator，导出两个 C 符号：

```cpp
void* kv_fabric_alloc_fn(ssize_t nbytes, int device, void* stream);
void  kv_fabric_free_fn(void* ptr, ssize_t size, int device, void* stream);
```

分配走 VMM 五步：`cuMemGetAllocationGranularity` → `cuMemCreate` → `cuMemAddressReserve` → `cuMemMap` → `cuMemSetAccess`，关键是：

```cpp
prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_FABRIC;  // <-- KEY
```

与 Mooncake 的 host fabric allocator 的差别：

| | vLLM `kv_fabric_alloc` | Mooncake `host_fabric_allocator` |
| --- | --- | --- |
| location | `CU_MEM_LOCATION_TYPE_DEVICE` (显存) | `CU_MEM_LOCATION_TYPE_HOST_NUMA` (宿主 DRAM) |
| access desc | 1 个(当前 device) | `device_count + 1` |
| size 记录 | 全局 `unordered_map` + mutex | `cuMemGetAddressRange` 回取 |
| 失败处理 | `abort()` | 返回 `nullptr` + `LOG(ERROR)` |

编译：

```bash
g++ -shared -fPIC -O2 -o kv_fabric_alloc.so kv_fabric_alloc.cpp \
    -I/usr/local/cuda/include -L/usr/local/cuda/lib64/stubs -lcuda
```

### 2.3 `apply_kv_fabric_patch.sh`

幂等脚本，在 `vllm serve` **之前**跑。改 `vllm/v1/worker/gpu/attn_utils.py`：

1. 注入 `kv_fabric_mem_pool()` 辅助函数 —— 用 `CUDAPluggableAllocator` 包住 `.so`，建 `torch.cuda.memory.MemPool`；`VLLM_KV_CACHE_FABRIC_ALLOC != 1` 时返回 `contextlib.nullcontext()`。
2. 把 `_allocate_kv_cache` 里两处 `torch.zeros(...)`(packed 和 plain 两条分支)包进 `with kv_fabric_mem_pool():`。
3. 清掉 `__pycache__` 里的 stale bytecode。

脚本用 `assert anchor in src` 卡住三个锚点，vLLM 版本漂移改了这些代码会**直接失败而不是静默不 patch**。锚点对应的 vLLM 路径是 `/usr/local/lib/python3.12/dist-packages/vllm`(写死)。

### 2.4 vLLM 侧所需 env

| env | 值 | 作用 |
| --- | --- | --- |
| `VLLM_KV_CACHE_FABRIC_ALLOC` | `1` | 开启 fabric KV 分配(配合上述 patch) |
| `NVIDIA_IMEX_CHANNELS` | `0` | container toolkit 据此注入 `/dev/nvidia-caps-imex-channels/channel0`。**GB300 上缺它 NCCL MNNVL 直接失败** |
| `NCCL_MNNVL_ENABLE` | `1` | 开启 NCCL 多节点 NVLink |

成功的日志标志：

```
[kv-fabric-patch] compiled /tmp/kv_fabric_alloc.so
[kv-fabric-patch] attn_utils.py patched
KV cache: using fabric-VMM MemPool (MNNVL exportable)
```

### 2.5 实测数字

参考部署 `yaochen-dev-decode-final.yaml` 的注释里记录了两个数字：

| 路径 | 带宽 | 出处 |
| --- | --- | --- |
| KV 经 UCX / NVLink (MNNVL) 跨节点 | 599 GB/s | YAML 注释"实测 599 GB/s" |
| rail-injector 注入网口(非 hostNetwork) | 48.7 GB/s | YAML 注释"实测 48.7GB/s，与 hostNetwork 持平" |

这两个数字来自那份 YAML 的注释，**不是本轮实测**。除此之外本文档不提供任何带宽或延迟数字。

---

## 三、当前边界

| 能力 | 状态 |
| --- | --- |
| 机架感知放置(Store 写路径) | 已实现，有单测 |
| 机架感知副本选择(Store 读路径) | 已实现，有单测 |
| 机架感知选路(TE `selectTransport`) | 已实现，有单测 |
| VRAM fabric 分配 | 已实现，env 开关，config 层有单测 |
| host fabric / EGM 分配 | 已实现，env 开关，config 层有单测 |
| 真实 NVLink 拓扑上的端到端验证 | **未做** —— 需要多节点 NVLink domain |

`host_fabric_allocator.cpp` 的 CUDA 分支只过了编译器，`cuMemCreate(HOST_NUMA + FABRIC)` 在真实硬件上的行为未验证。
