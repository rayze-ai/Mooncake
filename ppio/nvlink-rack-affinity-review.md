# NVLink 机架感知：需求梳理、实现评审与后续计划

> 讨论稿。记录 2026-09-20 一轮梳理的结论，包含理论判据、对
> `yaochen/topology-aware` 分支的评审、已修的缺陷和剩余工作。
>
> 与既有文档的关系：`20260918.md` 是方案设计与可行性分析，本文是在它之上
> 的一次**纠偏 + 代码评审**。两者结论有冲突的地方，以本文为准，冲突点在
> 第 6 节逐条列出。

## 1. 一句话结论

放置层（master 选点、reader 选副本）已经可用；**NVLink 还没有进入数据路径，
两条目标需求都未达成**。另外在「已声称可上线」的机架感知部分里发现 5 个独立
缺陷，其中一个会在 master failover 后让 strict 模式的写入全部失败，另一个会让一次
滚动升级静默作废机架亲和性。

## 2. 理论判据：什么决定一块内存能不能走 NVLink

这一节推翻一个流传的说法：「Mooncake 用 pinned DRAM，所以用不了 NVLink」。
**结果在今天的默认配置下是对的，判据是错的**，而判据错会导致后续决策跑偏。

### 2.1 真正的判据

NVLink 是**内存语义**互连，传的是 load/store/atomic，跑在一个物理地址空间上。
链路本身不关心目标物理页是 HBM 还是 DRAM。能不能走，取决于两个正交条件：

**条件一：这块内存在物理上挂在 NVLink fabric 上吗。**

| 机器形态 | host DRAM 是否在 NVLink 上 |
| --- | --- |
| x86 + PCIe 挂 GPU（传统 HGX） | **否**。NVLink 只连 GPU↔GPU，DRAM 在 PCIe/DDR 一侧，fabric 里不存在这条边 |
| CPU-GPU 相干互连（Grace-Blackwell，NVLink-C2C） | **是**。LPDDR 被拉进 GPU 可寻址的相干地址空间 |

这跟 ISA 无关。反例是 POWER9 + Volta（IBM AC922），CPU↔GPU 就是 NVLink 2.0。
判据是「CPU 侧有没有实现一条与 GPU 相干的 NVLink 级端口」——Intel/AMD 没有，
Grace 有。

**条件二：这块内存被映射/导出到 fabric 地址空间了吗。**

有资格不等于可达。远端要访问，需要稳定物理后备 + 进 fabric 可见页表 +
可导入的句柄。跨节点还要一层授权边界（NVL72 上是 IMEX domain）。

### 2.2 pin 是个红鲱鱼

pin 和 NVLink 可达性是两件正交的事：

- `cudaHostRegister` / `mlock` 解决的是「给 DMA 引擎一个不会跑掉的物理地址」，
  服务的是 **RDMA 路径**。
- NVLink fabric 要的是 VMM 分配 + 可导出的物理句柄 + 映射进 fabric 地址空间。

交集只有「物理内存不动」，充分条件完全不同。所以 pinned DRAM 不是走不通的
原因，pin 了也不代表走得通。

### 2.3 显存也不自动可达

这是同一个误区的另一面。实测判据在 `nvlink_transport.cpp:997`：fabric 模式下
`registerLocalMemory` 第一步是 `cuMemRetainAllocationHandle(addr)`，失败就打
WARNING 然后 `return 0`——**注册「成功」但什么都没注册，静默**。这个调用只认
VMM 分配：

| 内存来源 | 能否导出 fabric handle |
| --- | --- |
| `aligned_alloc` / numa / hugepage mmap（Store 默认） | 不能 |
| `cudaHostRegister` pinned DRAM | 不能 |
| **`cudaMalloc` 的显存** | **也不能** |
| `cuMemCreate` + `CU_MEM_HANDLE_TYPE_FABRIC` | 能 |

所以「DRAM 不行 / 显存行」这个二分不成立，两边都取决于用哪个分配器。

准确的说法是：**Store 现在走不了 NVLink，是因为它的分配器没走 VMM fabric
路径，不是因为它用 DRAM。**

### 2.4 host DRAM 走 NVLink 是可行的（EGM）

`CU_MEM_LOCATION_TYPE_HOST_NUMA` 的 fabric 分配就是 EGM。TENT 里已实现，
`mnnvl_transport.cpp:70` 的 `buildEgmAllocationProp()`，注释直接点名了这个场景：

> host DRAM allocated through the CUDA VMM API with a HOST_NUMA location. Such
> allocations can be exported as fabric handles and read/written by GPUs on
> other nodes of a multi-node NVLink domain, so host-resident data (e.g. a
> CPU-side weight or **KV cache**) moves over NVLink instead of the NIC.

Store 没接这条路，三道独立的坎：

1. Store 的 DRAM 分配路径不是 VMM 分配。
2. classic `nvlink_transport` 只支持显存——`HOST_NUMA` 在该文件出现 **0 次**，
   三处 `CU_MEM_LOCATION_TYPE` 全是 `DEVICE`（`:1128,1223,1285`）。
3. EGM 只在 TENT，默认关（`transports/mnnvl/egm`，需 `MC_MNNVL_EGM=1`），
   入口 `allocateLocalMemory` 在 classic `TransferEngine` 上不存在。

值得注意：`registerLocalMemory` 的 fabric 分支**不看内存位置**，
retain → `cuMemGetAddressRange` → `cuMemExportToShareableHandle` 全程没有
location type 判断。**缺口纯在分配侧**，注册/导出/导入链路不用动。

### 2.5 带宽序关系

即使 EGM 通了，DRAM-over-NVLink ≠ HBM-over-NVLink。瓶颈在网关之后：
远端 GPU → NVSwitch → 目标节点 GPU → C2C → Grace → LPDDR。

```
本地 HBM > HBM over NVLink > DRAM over NVLink（受 C2C/LPDDR 限） >> DRAM over RDMA
```

数量级参考（随代际变，不要当精确值）：C2C 约 900 GB/s 双向、LPDDR 约 500 GB/s
量级、per-GPU NVLink 1.8 TB/s、跨机架 RDMA 约 100 GB/s。关键是最后那个 `>>`：
DRAM-over-NVLink 拿不到满带宽，但相对跨机架 RDMA 仍有接近一个数量级的收益。

NUMA 细节：copy 应由**属主 socket 的 GPU** 发起，否则多一跳 CPU-to-CPU，
把本来最窄的那段再窄一次（`findDeviceOnNuma()` 的注释即为此）。

## 3. 需求的准确表述

原始表述有两处需要修正。

### 3.1 「同机器」应为「同机架」

这条影响收益范围，比较关键。NVL72 一个机架 = 一个 NVLink 域 = 一个 IMEX
domain（18 节点 / 72 GPU）。MNNVL 的 fabric handle 本来就是为跨节点导入设计
的，同机架不同节点照样走 NVLink。要求「同机器」会把可用范围从 18 台压到 1 台。

只有 `nvlink_intra` 协议才需要同机器——它靠 `cudaIpcGetMemHandle`，跟 fabric
handle 是两套机制。

### 3.2 「D 从 P 拉取」严格说不成立

KV 不在 P 上。P 把 KV 写进 store segment，D 从副本所在的 segment 读。只有
store server 与 P 同机部署时两者才等价。判断走什么传输，看的是**副本落在哪个
segment、那个 segment 的内存怎么分配的**，跟 P 在哪没关系（P 只通过 master 的
放置策略间接影响副本位置）。

### 3.3 修正后的判据

只需两问，顺序固定：

1. **源和目标在同一个 NVLink domain 吗？** 不在 → 只能 RDMA/RoCE，无第二选择。
   domain 边界是物理的，软件绕不过。
2. **该 KV 所在内存导出到 fabric 了吗？** 是 → NVLink；否 → RDMA。

注意第二问里**没有出现 DRAM 还是显存**。

### 3.4 目标矩阵

写入侧（P → store）：master strict 模式只在本机架 segment 分配，放不下则
`NO_AVAILABLE_HANDLE` + 触发淘汰，不溢出到其他机架。

读取侧（D → 副本）：

| 副本位置 | segment 内存 | 传输 | 状态 |
| --- | --- | --- | --- |
| 同机架 | VRAM + fabric | NVLink | 分配侧已实现；**选路层未实现** |
| 同机架 | host DRAM | RDMA | 今天如此 |
| 同机架 | host DRAM + EGM | NVLink | 路线 B，未实现 |
| 跨机架 | 任意 | RDMA / RoCE | 已实现（同机架优先 + 跨机架 fallback） |

## 4. 目标达成情况：0 / 2

两条目标**都**依赖选路层，而选路层是零。

**写入走 NVLink：未达成。** 已导出 ≠ 会走。`protocol_priority()`
（`multi_transport.cpp:615-627`）没有 nvlink 分支，落到 `0`，rdma 是 `2`，
NVLink 永远选不中。fabric 导出成功了也照样走 RDMA。

**跨 domain 退 RDMA：未达成，且比未实现更糟。** 在 nvlink 协议下不是退化成
RDMA，是**直接失败**：transport 安装是排他 if/else
（`transfer_engine_impl.cpp:410-454`），设了 `MC_FORCE_MNNVL` 的节点根本没装
rdma，没有东西可回落。`isLocalIpcReachableTarget()` 的门控名单只有
`hip`/`musa`/`shm`，nvlink 不在里面，跨机架不会被跳过。

这部分 `20260918.md` §8.4 与 `README.md`「当前能力边界」已如实记录，不算隐瞒。

## 5. 代码评审：5 个缺陷（文档未记录的部分）

### 5.1 严重：rack_id 不进快照，failover 后 strict 写入全停

`Serializer<MountedSegment>::serialize`（`serializer.cpp:919`）pack 9 个字段，
`rack_id` 不在其中；`store_resource_snapshot_codec.cpp:34` 同样 9 个，也没有。

快照恢复后每个 segment 的 `rack_id` 都是空串，后果连锁：

- 恢复路径的 `AddRackSegment` 变 no-op（内部 `if (!rack_id.empty())` 直接返回）
- 恢复路径上没调 `SetRackId`——它只在 `MountSegment` 里调
- **strict 模式**：`GetRackSegmentNames` 返回空 → 所有 serving segment 进
  `excluded_segments` → `rack_serving_names = 0` → **每个 Put 都
  `NO_AVAILABLE_HANDLE`，且不会自愈**，直到所有 segment 重新 mount
- soft 模式：静默退化成无机架偏好
- 读取侧：descriptor 的 rack 全空 → 所有副本看似远端 → 同机架优先失效

即 master 一次 failover 就把 prefill 写入全打死。`protocol` 字段同样没序列化
（既有问题），但后果远没这么大——strict 把「降级」放大成了「全停」。

### 5.2 严重：struct_pack 字段追加会破坏滚动升级

三处注释写着「Appended at the end to keep struct_pack field order stable」
（`allocator.h`、`types.h`、`replica.h`）。**这个判断是错的。**

struct_pack 的兼容性不靠字段顺序，靠**类型哈希**——对全部成员类型算一个 hash
做校验。往 `YLT_REFL` 里加普通字段会改变 hash，新旧两端反序列化直接失败。

仓库自己有正确约定，`metadata_store.h:39,102-104`：

```cpp
struct_pack::compatible<std::string, 1> group_id;
struct_pack::compatible<ObjectDataType, 1> data_type;
struct_pack::compatible<bool, 1> hard_pinned;
```

受影响三处，都跨 client↔master 线缆：

| 结构体 | 过线方式 |
| --- | --- |
| `Segment` | MountSegment RPC |
| `AllocatedBuffer::Descriptor` | GetReplicaList 响应 |
| `ReplicateConfig` | coro_rpc 参数（无 `YLT_REFL`，struct_pack 自动反射聚合成员） |

**握手检查挡不住这个问题（已取证）。** `master_client.cpp:500-506` 对版本不符是
**硬拒绝**：`ServiceReady()` 返回 `GetMooncakeStoreVersion()`，不等则 `Connect()`
直接 `INVALID_VERSION`。但这个版本串是 **冻结的**：
`mooncake-store/CMakeLists.txt:1` 的 `project(MooncakeStore VERSION 2.0.0)` 自
2025-11-24 引入检查的 #1061 以来未动过（期间 1583 个 commit、13 次 wheel
版本发布）。wheel 的 `0.3.13` 只进 `MOONCAKE_DISPLAY_VERSION`，注释明确写着
“does not affect RPC compatibility”。

所以跨发布的 client 与 master 都声称 `2.0.0`，握手必然通过。结论：
**无版本门禁可以拦住 wire 格式不兼容的对等体**，该检查只能拦住从未发生过的
主版本跳变，它提供的是虚假的安全感。类型哈希一变，现象是握手成功、第一个
带该结构体的 RPC 反序列化失败——比直接拒连难排查得多。

对照：master 侧的两个版本门禁是**真生效**的，都按严格相等拒绝：快照
`kSnapshotSerializerVersion = "1.0.0"`（`master_snapshot_repository.cpp:263`）、
OpLog `kOpLogBatchRecordSchemaVersion = 1`（`oplog_batch_codec.cpp:227`）。两者都是
master↔master（failover / standby），不走 client 路径。

### 5.3 轻：CXL 分支加了索引但没设 allocator 的 rack

`segment.cpp` CXL 分支调了 `AddRackSegment`，但该分支用共享的
`cxl_global_allocator_`，全程没调 `SetRackId`。master 侧认为同机架、reader 侧
看到空 rack 当远端——两侧不一致。

CXL + NVLink 本非有意义的组合，实际影响很小。但它暴露一个设计脆弱点：
**rack_id 挂在 allocator 上**，而 `cxl_global_allocator_` 被多 segment 共享。
若这些 segment 分属不同机架，`SetRackId` 设成任何值都是错的。

### 5.4 无 fabric 支持的机器会在 mount 处失败

`allocatePinnedLocalMemory(size, alignment)` 在 `!supportFabricMem()` 时回落
`cudaMalloc`，对齐不满足只打 WARNING 就返回指针。但该指针会流到
`MountSegment`，那里有硬检查：`buffer % Slab::kSize` 不为 0 直接
`INVALID_PARAMS`。`cudaMalloc` 不可能保证 16 MB 对齐，所以这条「回落」实际是
**先警告、后在 mount 处失败**，不是可用降级。中间状态最难排查。

### 5.5 滚动升级会静默作废机架亲和性

滚动升级不只是版本兼容问题，它有一套真实的数据面原语，而**这套原语不认识
rack_id**。

Store 节点下线走 drain：`GracefulUnmountSegment`（`master_service.cpp:3117`）把
segment 置为 `GRACEFULLY_UNMOUNTING`、摘掉 allocator、
`SetAllocatable(false)`，但**保留 metadata 让存量数据继续可读**，到 grace
period 才真删。配套有 admin HTTP 接口
（`/api/v1/drain_jobs` 的 create / query / cancel）做副本迁移。注意
`PrepareGracefulUnmountSegment` 里已经正确调了 `RemoveRackSegment`，索引维护没问题。

问题在迁移的**目标选择**：`SelectDrainTargetForKey`
（`master_service.cpp:13793`）的筛选条件只有「非源 segment / 不与现有副本重合 /
allocatable / 有容量」，打分只用 `used / capacity` 利用率。**全程不看 rack_id。**

后果：一次 store 节点滚动升级后，原本以 `strict_rack` 写入、保证全部副本在
rack0 的 key，副本可能落到 rack1。数据仍可读（不是正确性 bug），但：

- strict 模式在 Put 时提供的放置保证被**事后作废**，且无任何日志提示
- 读取侧 `SelectBestReplica` 的同机架层随之失效，退回 RDMA —— 正是本项目要消除的
  情况
- 这是静默的：写入时的保证与运维动作分属两个子系统，没人校验二者一致

根因是 master 不保留原始 `ReplicateConfig`。`ObjectMetadata`
（`object_metadata.h:43`）存了 client_id、size、data_type、group_id、tenant_id、
soft/hard pin 等，**没有 rack_id 也没有 strict_rack**。所以 drain 想遵守机架约束
也无从得知该 key 原本要求什么。

同一类缺口也在动态复制路径上：`ReplicaActionProposal` 已经有
`requester_domain` / `target_domain` 字段（`rpc_types.h:20-34`），但
`SelectDynamicReplicaPlan`（`master_service.cpp:9207`）把 `target_domain` 收下后
**从不用它筛选候选**，只在 `:9349` 记进 plan、在 `:9410` 做提案去重比对。也就是
说「domain」这个概念已经铺到 RPC 层但没接放置层，而它正是 rack_id 最自然的落点。

修法有两档，建议先做第一档：
1. `SelectDrainTargetForKey` 增加「优先同 rack 的候选」，无同 rack 候选时记
   WARNING 再跨机架 —— 不需要持久化 config，用现有副本所在 segment 的 rack 当
   参照即可。
2. 若要恢复 strict 的严格语义，需把 `rack_id` / `strict_rack` 持久化进
   `ObjectMetadata`，drain 与动态复制都据此约束（并顺势把 `target_domain` 接上）。
   这会扩大改动面，且要同步快照/oplog 格式，建议与决策 #4 一起考虑。

### 5.6 两个运维陷阱

**改 rack_id 需先 unmount。** `MountSegment` 的幂等检查和
`ValidateRemountSegment` 都比较 `rack_id`，改 `MOONCAKE_RACK_ID` 后重启会拿到
`INVALID_PARAMS`。行为本身是对的（防身份漂移），但要写进部署文档：写错机架
id 想改正、或节点在机架间迁移，都会撞上。

**`SelectBestReplica` 的默认参数是陷阱。** 第三参数原有默认值 `= ""`，当前
调用点都正确传了，但以后新增调用点漏传会**静默丢掉机架亲和性**，无任何提示。

### 5.7 性能小点

`GetRackSegmentNames` 每次 Put 全表扫 `segment_rack_by_name_`，且在
`ScopedAllocatorAccess` 的 shared lock 内。72 节点规模无所谓，但它在 Put 热
路径上。规模上去后维护一个 `rack_id → names` 反向索引即可。

### 5.8 做对了的部分

- strict/soft 语义划分正确；`strict_rack=true` + 空 `rack_id` 打 WARNING 退非
  strict，不静默改行为。
- 排除与偏好的冲突：`allocation_strategy.h:345` **排除先判**，strict 下
  host-local 偏好与机架排除冲突时排除赢——正确。
- `SelectBestReplica` 分层插入位置正确，同机架命中时 `continue` 跳过
  `first_memory` 赋值，保证同机架不被远端评分绕过。空 `local_rack_id` 逐字节
  等价历史行为。
- `free_memory` 与 `allocate_vram_memory` 共用 `use_fabric_vram()` 判定、环境
  变量读一次缓存在函数内 `static`——避免中途改环境变量导致分配器与释放器不
  匹配。A3 的泄漏隐患修得干净。
- 对齐用 `max(granularity, alignment)`，非 2 的幂时回退 granularity 并告警而
  不是静默放宽。
- `MC_STORE_VRAM_FABRIC` 用按值解析而非「存在即生效」，理由（避免 `=0` 被当
  成 opt-in 从而选中静默失败模式）站得住。

## 6. 与 `20260918.md` 的冲突点

| 位置 | 原文 | 更正 |
| --- | --- | --- |
| §4.3 表格 | 暗示「显存能导出 fabric」 | `cudaMalloc` 的显存**也不能**；判据是 VMM 分配，不是内存位置 |
| §8.4 第 4 项 | 「能直接复用本次已落地的 rack_id」 | 不是复用。`rack_id` 只活在 Store 的 `Segment` 和副本 descriptor 里，**TE 自己的 segment/buffer 元数据没有这个字段**，门控在 TE 内部做，看不到 Store 的 rack_id。需先打通到 TE 元数据层 |
| 三处代码注释 | 「Appended at the end to keep struct_pack field order stable」 | 顺序不是兼容性的依据，类型哈希才是。见 5.2 |
| §7 / README | 机架感知「已实现可用，可以直接上线」 | 带 5.1 上线，一次 master failover 即写入全停 |

## 7. 本轮已做的修改

> 状态：已落盘，**未编译**。见第 9 节。

| 项 | 改动 |
| --- | --- |
| P0-1 | 两个 codec 的 `rack_id` 序列化，数组 9→10，`size >= 10` 门控保留旧快照可读；恢复路径补 `SetRackId` |
| P0-2 | 语义不变（见下），仅加一条 ERROR 区分「全空 rack 索引」与普通容量不足 |
| P0-3 | 三个结构体改 `compatible<T,1>` + 访问器（`rack_id()` / `RackId()` / `IsStrictRack()`），调用点跟随 |
| P0-4 | 无 fabric 且对齐不满足时 `cudaFree` + 返回 nullptr，在正确的位置快速失败 |
| P0-5 | CXL 不进机架索引，并注明未序列化的 `protocol` 在恢复路径上为何不是漏洞 |
| 加固 | 去掉 `SelectBestReplica` 的默认参数（17 个测试调用点显式传 `""`）；`Segment::operator==` 显式展开 |
| 测试 | 新增 4 个序列化 round-trip 用例（带 rack / 无 rack / 9 字段旧格式 / 显式空） |

**P0-2 的决定：保持 fail-closed。** 空 rack 不进候选集，即「无机架身份的
segment 不属于写入方的机架，因此不是候选」。这是既有语义，本轮未改。曾考虑
fail-open（索引全空时退非 strict，避免全停），经确认后放弃——strict 的放置保证
不应被静默放弃。代价是 5.1 那条链路仍可能在旧快照上复现，因此加了 ERROR 日志
让它可区分。

赋值点的 unset 语义已统一：`allocator.cpp` 的 `get_descriptor()` 与
`client_service.cpp` 的 `MountSegmentAndGetId()` 都只在 rack 非空时才 engage
optional，让无机架的 client / segment 发送真正的 unset 而非
engaged-but-empty。通过 `RackId()` 读两者都空，不影响正确性，只是语义更干净，
也与「未配置 rack 的旧版本对端发什么」保持一致。

### 7.1 建议考虑的结构性修改

`rack_id` 现存两处（`segment_rack_by_name_` 与 `allocator->rack_id_`），靠手工
同步。5.1 的后半段（恢复路径漏 `SetRackId`）和 5.3 都是这个重复的症状。

更干净的做法：descriptor 的 rack 只从 `segment_rack_by_name_` 出，master 组副本
列表时填入，allocator 上不存。单一数据源，加路径不会漏。未追 `get_descriptor()`
全部调用点，不确定那些位置能否拿到 rack 索引，故仅作方向提出，不是 P0 前置。

中间方案：把 rack_id 变成 `CreateBufferAllocator` 的构造参数而非事后 setter，
编译器会强制每条路径表态。注意反序列化路径上 allocator 在 `array[7]` 构造、
rack_id 在 `array[9]`，需先读后者再构造。

## 8. 剩余工作与建议顺序

| 阶段 | 内容 | 可验证性 | 状态 |
| --- | --- | --- | --- |
| 0 | 5.1 / 5.2 / 5.4 / 5.5 | 单测可覆盖 | 本轮已改，待编译验证 |
| 1 | 选路层：nvlink 进 `protocol_priority()` + **机架可达性门控** | 单测可覆盖，不需 NVL72 | 未做 |
| 2 | 双 transport 并存（打破排他 if/else）+ `ENABLE_MULTI_PROTOCOL` + 双协议注册 | 需多节点 | 未做 |
| 3 | 真机验证（见 8.2） | 需 NVL72 | 未做 |
| 4 | 路线 B：EGM，host DRAM 也能 NVLink | 按需 | 未做 |

阶段 0 优先于一切 NVLink 工作：它修的是**已经声称可上线的部分**。

阶段 1 里**机架可达性门控**应最先做——它让「跨机架不会误选 NVLink」变安全，
是后面几步的前提，且纯单测可覆盖。注意当前「阶段 2 先于阶段 1 落地」
（fabric 分配已实现、门控未实现），所以 `MC_STORE_VRAM_FABRIC` 目前**只应在
隔离节点上验证**。

### 8.1 门控怎么做：两条路（需决策）

`rack_id` 目前不在 TE 元数据里（见第 6 节），所以两条路都要先打通字段或换判据。

**路线 甲：配置驱动。** 给 TE 的 `BufferDesc` / segment desc 加 rack_id，门控
比较本地与目标 rack。缺点：多一份配置依赖。`MOONCAKE_RACK_ID` 配错（如两个物理
机架填了同一个 id），门控会放过实际不可达的目标，然后 fabric handle 导入失败且
不回落——回到今天这个硬失败。

**路线 乙：fabric 自证。** 首次访问某目标 buffer 时尝试
`cuMemImportFromShareableHandle`，成功则缓存「该目标走 NVLink」，失败则缓存
「退 RDMA」。好处：**配错 rack_id 只导致放置不优，不导致传输失败**，系统自纠。
代价：首次访问多一次 import 尝试的延迟，且要把失败路径处理干净（别把 import
失败当传输错误上报）。

**已决策（2026-09-21）：走路线甲，配置驱动。** 我原先倾向乙，理由是失败模式代价太高；
决策为甲，因此必须用工程手段把那个代价补掉，否则「配错一个环境变量 → 跨机架读取
不可用」的风险是实打实的。路线甲落地的三个前置，缺一不可：

1. **rack_id 打通到 TE 元数据层。** 现有 `rack_id` 只在 Store 侧，TE 看不到（见第 6 节），
   这是前置工作而非复用。
2. **import 失败必须能回落 RDMA。** 打破 `transfer_engine_impl.cpp:410-454` 的排他
   if/else，让 nvlink 与 rdma 并存。这使原「阶段 2」从优化升级为**必需**。
3. **启动期一致性校验。** 检测同一 rack_id 下节点两两之间 fabric 是否真可达，把配置
   错误暴露在启动阶段，而不是首次跨机架读取时。

第 3 项实质是把路线乙的探测保留为**校验手段**（启动跑一次），而非**选路依据**（每次
传输动态判定）：保留配置驱动的确定性，同时不让一个手填的环境变量成为单点。

详见 `fixed.md` 第 1 节。

### 8.2 必须在真机（NVL72）验证的项

- `CU_DEVICE_ATTRIBUTE_HANDLE_TYPE_FABRIC_SUPPORTED` 在全部 GPU 上为真
  （否则 `supportFabricMem()` 返回 false，静默回落 `cudaMalloc`）
- IMEX domain 覆盖整机架 18 节点，且与 `rack_id` 划分一致
- `cuMemGetAllocationGranularity` 的实际取值（决定对齐处理）
- 未设 `MC_USE_NVLINK_IPC`（一旦设置，`supportFabricMem()` 直接返回 false）
- 跨机架 get 确实回落 RDMA 而非报错

### 8.3 部署约束

- **GB300 是 Grace，aarch64**，镜像必须在 arm64 上构建
- **NVLink 节点不能设 `MC_STORE_USE_HUGEPAGE`**（存在即生效，设 `0` 也算开），
  否则分配在到达 VRAM 路径前被截走
- **生产节点一律不设 `MC_FORCE_MNNVL`**：设了就没有 RDMA，跨机架读直接失败且
  无回落
- strict 模式下 `replica_num` ≤ 本机架可服务 segment 数，否则分配必然失败
- strict 模式需按机架监控容量与淘汰率：单机架写满即失败，不跨机架兜底

## 9. 验证状态（重要）

**本轮所有 C++ 改动一行都没编译过。** 环境缺 glog / boost / ylt /
tl::expected 且无 apt 源，无法配置 CMake。

已做的验证：

- 读代码。5.1 的结论较硬（序列化字段能直接数出来）
- 独立 harness，`g++ -std=c++20 -Wall -Wextra`，13 项断言全通过：访问器语义、
  分层顺序（local > 同机架 > 远端、local NoF 仍优先、同机架优于远端 NoF、
  未完成副本跳过、空 rack 等价历史行为）

harness 用一个 stand-in 模拟 `compatible<T,N>`（继承 `std::optional<T>`、
继承构造但不继承 `operator=`）。**若真实实现与此不同，关于相等性与赋值的结论
会跟着变。**

harness 抓到一个真问题：派生自 `std::optional` 会使 `==` 有歧义，从而让
`Segment` 的 defaulted `operator==` 被**隐式删除**。今天没有整体比较
`Segment` 的地方，所以能编译过，但会在第一个使用者那里炸。已显式展开。

**仍需在有依赖的环境执行**：新增 4 个序列化用例、`vram_fabric_config_test`
（7 例）、机架改动的 11 个 C++ 用例，以及一次完整构建。

**5.2 的类型哈希结论是推理，不是实测。** 建议用两个版本的结构体做一次实际
序列化/反序列化对撞确认，比看代码可靠。

## 10. 待决策清单

| # | 问题 | 影响 |
| --- | --- | --- |
| 1 | ~~client↔master 滚动升级是否为支持的性质？~~ **已解决：是** | 5.2 为**强制项**。理由见 5.2 末尾：握手版本串冻结在 `2.0.0`，拦不住跨发布对接；且 `kv-cache-sharing-and-isolation.md:103` 明文假设新旧 release 共用同一 Store |
| 2 | ~~门控走路线甲还是乙（见 8.1）~~ **已解决：甲（配置驱动）** | 配错 rack_id 的后果是**传输失败**，因此 8.1 列的三个前置（rack_id 进 TE 元数据、import 失败回落 RDMA、启动期一致性校验）成为必做项，其中第 2 项把原「阶段 2」从优化升级为必需 |
| 3 | 本地 NOF_SSD vs 同机架 MEMORY 的优先级 | 当前 SSD 优先（沿用历史行为）。GB300 上同机架 NVLink DRAM 约 900 GB/s 量级 vs 本地 NVMe 约 10 GB/s 量级，可能不合理 |
| 4 | 是否做 7.1 的结构性修改（rack 单一数据源） | 决定 5.3 类问题会不会再长出来 |
| 5 | 是否投入路线 B（EGM） | 建议等选路层跑通、真机验证后再定。路线 A 剩余工作量全在选路层，而选路层是两条路**共用**的前置 |
| 6 | drain / 动态复制是否需要遵守机架约束（见 5.5） | 决定「strict 写入的放置保证」是否能在一次滚动升级后存活。第一档修法（drain 优先同 rack）代价小，建议直接做 |
