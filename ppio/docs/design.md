# 方案思路

## 1. 要解决什么

GB300 NVL72 的机架内有 NVLink 全互联，机架之间只有 RDMA。带宽差一个数量级以上。但 Mooncake 原来的放置和选路对这个差异是**无感的**：

- master 分配副本时只看容量，不看副本落在哪个机架；
- client 读副本时只区分 local / remote，同机架的远端副本和跨机架的远端副本一视同仁;
- TE 选路时 nvlink 甚至不在优先级表里。

结果是一次读可能本可以走 NVLink，却走了 RDMA。要拿到 NVLink 的带宽，需要**放置、选择、选路**三层都知道机架边界在哪。

第二个问题更隐蔽：**即使选中了 nvlink，内存本身也可能不可达**。NVLink 跨节点(MNNVL)要求内存导出 fabric handle，而 Store 默认用 `aligned_alloc`/`cudaMalloc` 分配，拿不到 handle。

## 2. 两条独立的线

这两个问题的解法是独立的，可以分开上线：

```
                    ┌─────────────────────────────┐
                    │  A. 机架感知(放置/选择/选路) │  纯逻辑，无编译开关
                    └─────────────────────────────┘
                                  │
                                  │ 有了它，nvlink 才会被选中
                                  ▼
                    ┌─────────────────────────────┐
                    │  B. fabric handle 分配       │  需 CUDA，env 开关
                    └─────────────────────────────┘
                                  │
                                  │ 有了它，选中的 nvlink 才真的能传
                                  ▼
                            NVLink 数据通路
```

**A 不依赖 B**：机架感知本身就有收益 —— 即使内存不是 fabric 的，把副本放在同机架也减少了跨机架 RDMA 流量。所以 A 建议先上。

**B 依赖 A**：没有机架感知，选路不会选 nvlink，fabric 内存分配了也用不上。

## 3. 机架边界为什么必须提前判定

这是整个设计里最关键的一个决定。

NVLink domain 的边界是**物理的**。一个机架上导出的 fabric handle，在另一个机架上无法导入 —— 这不是性能退化，而是硬失败。可选的两种处理方式：

| 方式 | 行为 |
| --- | --- |
| 事后重试 | 交给 nvlink → `submitTransfer` 里 fabric import 失败 → 换 transport 重试 |
| **提前判定** | 选路时就比较 rack_id → 不可达则直接选 rdma/tcp |

选了后者。理由：fabric import 的失败发生在 transfer 提交之后，错误信息是驱动级的、不透明的，而且已经付出了建立路径的开销。提前判定用一次字符串比较就能避免。

代价是需要一个**可信的 rack_id 来源**。这带来了三态设计。

## 4. 为什么是三态而不是布尔

```cpp
enum class RackReachability { Reachable, Unreachable, Unknown };
```

`Unknown`(任一侧 rack_id 未设)与 `Unreachable`(两侧明确不同)是两件不同的事：

- `Unreachable` 是**对拓扑的陈述**："我知道这两个机架不同"。此时无回落 transport 就该报错。
- `Unknown` 是**信息缺失**："部署没告诉我"。这可能是单机部署(根本没机架概念)、也可能是配置遗漏。

如果合成一个布尔，就必须二选一：

- 都当可达 → 配置遗漏的跨机架部署会静默走 nvlink 然后 import 失败，回到了要避免的情况；
- 都当不可达 → 单机部署、以及所有没配 rack_id 的存量部署，全部丢掉 nvlink。

三态让调用方**对两者都降级**(保守),但只有 `Unreachable` 在无路可走时报错。同时，同主机一律判 `Reachable` 而不看 rack_id —— 保证单机部署完全不受影响，这是向后兼容的关键。

## 5. strict 与 soft：谁来决定跨机架外溢

写路径有两种策略，区别在于**本机架满了怎么办**：

| | soft (`rack_id` 设了) | strict (`strict_rack=true`) |
| --- | --- | --- |
| 实现 | 同机架 segment 进 `preferred_segments` | 机架外 segment 进 `excluded_segments` |
| 本机架满 | 跨机架外溢，走 RDMA | 分配失败，触发淘汰 |
| 适用 | 默认。要带宽但不能丢可用性 | 明确不接受跨机架流量 |

strict 是"宁可淘汰也不跨机架"。这是个**运维决策**而非技术优选，所以做成显式开关，默认 soft。

strict 模式有两个诊断兜底，都是为了让配置错误可见：

1. `strict_rack=true` 但 `rack_id` 空 → 告警并退回 soft。否则每个写都失败，而原因("你开了严格模式但没给机架标识")从错误码里看不出来。
2. rack index **完全为空** → `LOG(ERROR)` 点名。这说明没有任何 segment 报告过 rack(配置问题，或 master 恢复了 pre-rack 快照)。它产生的 `NO_AVAILABLE_HANDLE` 和"容量真的耗尽"完全无法区分，必须显式说出来。

## 6. 读路径：同机架层插在哪

原来的副本优先级是 4 层，新增同机架层后：

```
local MEMORY
local NOF_SSD
同机架 MEMORY      ← 新增
remote MEMORY
remote NOF_SSD
LOCAL_DISK / DFS / DISK
```

**同机架层在 local NOF_SSD 之下**,这一点是刻意保持的 —— 本地 SSD 读仍然优先于同机架内存读。改这个顺序会改变该层级引入前的行为，超出了本次改动的范围。

无 rack 信息的副本(旧 master 返回的)按 remote 处理，所以空 `local_rack_id` 精确复现历史行为。`SelectBestReplica()` 的新参数**不给默认值**:忘记传会静默丢掉机架亲和性，而编译期抓不到 —— 要放弃就显式传 `""`。

## 7. fabric handle：为什么不能回落

`NvlinkTransport::registerLocalMemory()` 内部调 `cuMemRetainAllocationHandle()`。这个调用对非 VMM 分配的指针(`cudaMalloc`、`aligned_alloc`)会失败，而失败之后注册**返回成功但什么都没注册**。

后果链条：

```
aligned_alloc 的 buffer
  → registerLocalMemory 静默注册了空
  → MountSegment 成功
  → segment 出现在 master 的索引里
  → 远端选路选中它
  → 第一次读 miss
```

失败点离原因隔了五步。所以分配侧的设计原则是**宁可当场失败**：

- 驱动不支持 fabric handle → 返回 `nullptr`,不 fallback；
- 拿不到 host NUMA node → 返回 `nullptr`,**不猜 node 0**；
- 任一 VMM 调用失败 → 清理已分配的资源并返回 `nullptr`。

配套地，env 开关用**取值判定**而非存在判定(`MC_STORE_HOST_FABRIC=0` 明确是关)。这与 `MC_STORE_USE_HUGEPAGE`(存在即生效，设 `0` 也算开)相反 —— 后者那种语义在这里特别危险，因为静默 opt-in 选中的失效模式正是上面那条五步链。

## 8. VRAM fabric 与 host fabric 的分工

两个开关解决的是**不同层的内存**：

| | `MC_STORE_VRAM_FABRIC` | `MC_STORE_HOST_FABRIC` |
| --- | --- | --- |
| 内存 | GPU 显存 | 宿主 DRAM (EGM) |
| CUDA location | `DEVICE` | `HOST_NUMA` |
| 容量 | 受 HBM 限制 | 大得多 |
| 谁写 | GPU | **CPU** |
| access desc | 每个 device 一个 | 每个 device + host NUMA,共 `device_count + 1` |

host fabric 的 access descriptor 多一个 host 条目，是因为这块内存的访问模式不对称：CPU 写入(Store 往里 memcpy)、GPU 读取。两种 location type 都要授权。VRAM 不需要 host 条目。

host fabric 即 **EGM (Extended GPU Memory)** —— 宿主 DRAM 经 VMM 分配并导出 fabric handle,让远端 GPU 能经 NVLink 直接读。它的意义是把 NVLink 的低延迟扩展到**容量远大于 HBM 的一层**。

## 9. 编译条件为什么排除 USE_VRAM_SEGMENT

```cpp
#if defined(USE_CUDA) && !defined(USE_VRAM_SEGMENT)
#define MOONCAKE_STORE_FABRIC_HOST 1
```

host fabric 的分支在 `allocate_buffer_allocator_memory()` 的**最后一段**,即 `aligned_alloc` 回落之前。而 `USE_VRAM_SEGMENT` 构建永远走不到那里 —— 它在前面就 `return` 了(那个配置下所有 segment 都是设备内存，不论 protocol)。

编进去会是死代码，并且给人一种"两个开关可以同时生效"的错觉。

## 10. 线上兼容

rack_id 要加进两个已经在网上传输的结构：`Segment` 和 `AllocatedBuffer::Descriptor`。用 `struct_pack::compatible<std::string, 1>` 而非普通成员：

普通成员会改变 struct 的 type hash,新旧版本的 client 与 master 就无法交换 `MountSegment` 请求 —— 升级必须停机且顺序敏感。`compatible` 保持 hash 稳定，旧 peer 读到的就是"字段不存在",正好对应"无机架信息"的语义。

副作用：`compatible` 派生自 `std::optional`,会让 defaulted `operator==` 变歧义(从而隐式删除)。这个默认版本**在定义处能编过、在第一个调用方才炸**,所以手写了比较函数。手写版通过 `RackId()` 比较，顺带让"未设"与"显式空"相等 —— 这正是代码其余部分对"无机架"的定义。

## 11. 代码组织

按"大改动独立成文件、原有文件只做薄调用"组织：

| 新增(独立文件) | 行数 | 原有文件的改动 | 行数 |
| --- | --- | --- | --- |
| `multi_transport_locality.h` 判定纯函数 | 138 | `multi_transport.cpp` | +94 |
| `common/host_fabric_allocator.{h,cpp}` VMM | 256 | `client_buffer_allocation.cpp` | +153 |
| `config/host_fabric_config.{h,cpp}` env 解析 | 65 | `config.cpp` | +19 |
| `config/vram_fabric_config.{h,cpp}` env 解析 | 65 | `CMakeLists.txt` | +11 |

右列是 `main..HEAD` 两个 commit 的合计。其中 `client_buffer_allocation.cpp` 的 153 行拆开是：前一个 commit 的 VRAM fabric 分支 93 行，本轮 host fabric 60 行。

判定逻辑抽成 header-only 纯函数的直接好处是**可单测** —— IPv6 字面量、大小写、空白这些边界情况不需要起一个 transfer engine 就能覆盖。

`client_buffer_allocation.cpp` 里只有一个谓词加两个分支。谓词用 `static const bool` 缓存 env:

```cpp
static const bool enabled = HostFabricConfig::IsEnabledFromEnvironment();
```

一次进程内只读一次，保证一次分配和它后来的释放对"谁拥有这块内存"判断一致。释放端的分支必须与分配端**逐项镜像**(同样的 protocol 判断、同样的开关)—— 不镜像的后果是 `free()` 一个 VMM 映射，在 glibc 从未分发过的指针上 abort。

## 12. 与 vLLM 的边界

Mooncake 只管 Store 的 segment。**KV cache 是 vLLM 分配的**,默认走 PyTorch caching allocator → `cudaMalloc`,没有 fabric handle。

所以 NVLink KV 通路需要两侧配合：

| 侧 | 做什么 | 怎么做 |
| --- | --- | --- |
| Mooncake | segment 内存 fabric 化 | `MC_STORE_VRAM_FABRIC` / `MC_STORE_HOST_FABRIC` |
| vLLM | KV cache 内存 fabric 化 | pluggable allocator + 运行时 patch |

vLLM 侧不在本仓库，是对已安装 vllm 包的运行时 patch(`kv_fabric_alloc.cpp` + `apply_kv_fabric_patch.sh`),细节见 `change.md` §2。

## 13. 已知未覆盖

| 项 | 状态 |
| --- | --- |
| 真实 NVLink 拓扑端到端验证 | **未做**,需多节点 NVLink domain |
| `cuMemCreate(HOST_NUMA + FABRIC)` 硬件行为 | 只过了编译器 |
| 多 NUMA 节点上 host fabric 的 node 选择策略 | 取当前 device 的 `cudaDevAttrHostNumaId`,未验证多 socket 场景 |
| NVLink 进入跨机架数据路径 | 物理上不可能(fabric handle 跨 domain 不可导入),已在选路层显式降级 |

最后一项不是缺陷而是物理约束 —— 设计上的处理方式是提前判定并降级，见 §3。
