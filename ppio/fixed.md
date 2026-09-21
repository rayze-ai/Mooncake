# 机架感知 / NVLink：决策记录与改动清单

> 配套文档：`nvlink-rack-affinity-review.md`（理论判据、缺陷分析、可行性）。
> 本文只记**决定了什么**和**改了什么**，供 review 与交接用。
>
> 最后更新：2026-09-21

## 1. 已决策

### 决策 #1：client↔master 滚动升级是支持的性质 —— 是

**推论：wire 格式必须前后兼容，`struct_pack::compatible<T, N>` 是强制项，不是可选项。**

取证过程与结论：

- `master_client.cpp:500-506` 确有硬版本门禁：`Connect()` 调 `ServiceReady()` 取服务端
  版本，与本地 `GetMooncakeStoreVersion()` 不等则返回 `INVALID_VERSION`，连接失败。
- **但该版本串是冻结的。** `mooncake-store/CMakeLists.txt:1` 的
  `project(MooncakeStore VERSION 2.0.0)` 自 2025-11-24 引入该检查的 #1061 起未再变更，
  期间 1583 个 commit、13 次 wheel 版本发布。
- wheel 的 `0.3.13` 只进 `MOONCAKE_DISPLAY_VERSION`，`version.h.in:17-18` 注释明确
  写着 "Used for the --version flag only; does not affect RPC compatibility"。
- `docs/source/deployment/kv-cache-sharing-and-isolation.md:103` 明文假设新旧 release
  共用同一个 Mooncake Store。

所以跨发布版本的 client 与 master 都声称 `2.0.0`，握手必过，**没有任何门禁会拦住
wire 格式不兼容的对等体**。往 `YLT_REFL` 里加普通字段的后果不是"握手时被干净
拒绝"，而是握手成功、第一个携带该结构体的 RPC 反序列化失败 —— 比直接拒连难排查。

对照：master 侧两个门禁是真生效的，都按严格相等拒绝 —— 快照
`kSnapshotSerializerVersion = "1.0.0"`（`master_snapshot_repository.cpp:263`）、OpLog
`kOpLogBatchRecordSchemaVersion = 1`（`oplog_batch_codec.cpp:227`）。两者都走
master↔master（failover / standby），不经 client 路径。这也解释了
`struct_pack::compatible<>` 这个约定的来源：它随 HA/快照工作（#3447、#2826）进来，
服务的是快照/oplog 前向兼容。

### 决策 #2：NVLink 选路门控走配置驱动（路线甲）

在目标 buffer 的 TE 元数据里带 rack_id，门控比较本地 rack 与目标 rack，同 rack 才允许
选 NVLink。**不采用** fabric 自证（尝试 `cuMemImportFromShareableHandle` 探测可达性）。

需要明确接受的代价：**`MOONCAKE_RACK_ID` 配错会导致传输硬失败，不是降级。** 两个不同
物理机架被填了同一个 id 时，门控会放过实际不可达的目标，随后 fabric handle 导入失败，
而当前没有回落路径（见 `nvlink-rack-affinity-review.md` 第 4 节：transport 安装是排他
if/else，设了 `MC_FORCE_MNNVL` 的节点根本没装 rdma）。

因此路线甲落地时必须同时做这三件事，缺一不可：

1. **rack_id 打通到 TE 元数据层。** 目前 `rack_id` 只活在 Store 的 `Segment` 和副本
   descriptor 里，TE 自己的 segment / buffer 元数据没有这个字段，门控在 TE 内部做，
   看不到 Store 的 rack_id。这是路线甲的前置工作，不能复用现有字段。
2. **import 失败必须能回落 RDMA。** 打破 `transfer_engine_impl.cpp:410-454` 的排他
   if/else，让 nvlink 与 rdma 并存；否则配错 rack_id 的后果从"慢"变成"不可用"。
3. **启动期一致性校验 + 告警。** 至少检测"同一 rack_id 下的节点两两之间 fabric 是否
   真的可达"，把配置错误暴露在启动阶段而不是首次跨机架读取时。

第 3 项实质上是把路线乙的探测逻辑保留为**校验手段**（启动时跑一次），而不是**选路
依据**（每次传输动态判定）。这样保留了配置驱动的确定性，同时不让一个手填的环境变量
成为单点。

## 2. 本轮代码改动

> 状态：**已编译、已跑测试，68 个用例全绿**。见第 4 节。

### P0-1（严重）rack_id 进快照 —— 修 master failover 后 strict 写入全停

两个 codec 都漏了 `rack_id`，快照恢复后每个 segment 的 rack 都是空串，strict 模式下
`GetRackSegmentNames` 返回空 → 所有 serving segment 进 `excluded_segments` →
**每个 Put 都 `NO_AVAILABLE_HANDLE` 且不会自愈**，直到所有 segment 重新 mount。

| 文件 | 改动 |
| --- | --- |
| `src/serialize/serializer.cpp` | `pack_array(9)` → `10`，两个分支都 pack `segment.RackId()`；读侧 `if (obj.via.array.size >= 10)` 门控 |
| `src/ha/snapshot/store_resource_snapshot_codec.cpp` | 同上 |
| `src/segment.cpp` | 恢复路径补 `AddRackSegment(...)` 与 `buf_allocator->SetRackId(...)` |

`size >= 10` 门控保证旧快照仍可读（segment 报告为无机架）。恢复路径必须同时补
`SetRackId`：rack 索引只驱动 master 侧放置，而 reader 从 buffer descriptor 拿 rack，
后者读的是 allocator —— 恢复出来的 allocator 不带 rack，它服务的每个副本都会被看成
跨机架。

### P0-2（严重）空 rack 索引的可诊断性 —— 语义按你的决定保持不变

保持 **fail-closed**：空 rack 不进候选集，即"无机架身份的 segment 不属于写入方的机架，
因此不是候选"。曾考虑 fail-open（索引全空时退非 strict 以避免全停），经你确认后放弃 ——
strict 的放置保证不应被静默放弃。

仅在 `src/master_service.cpp` strict 分支内增加一条 ERROR，把"全空 rack 索引"与
"普通容量不足"区分开：

```
key=..., strict_rack with rack_id=... but the master knows of no rack-bearing
segment at all; every memory allocation will fail. Check that store nodes set
rack_id, and that they remounted after a master restored a pre-rack snapshot.
```

代价：P0-1 那条链路在旧快照上仍可能复现，但现在日志可区分。

### P0-3（严重）wire 兼容 —— 落实决策 #1

三处结构体都跨 client↔master，原注释"Appended at the end to keep struct_pack field
order stable"判断有误：struct_pack 的兼容性不靠字段顺序，靠**类型哈希**，往
`YLT_REFL` 加普通字段会改变 hash，新旧两端反序列化直接失败。

**已实测确认（2026-09-21，真实 ylt 头文件 + `g++ -std=c++20`）。** 用同一结构体的三个
版本（旧 / 追加普通字段 / 追加 `compatible<std::string,1>`）做实际
serialize→deserialize 对撞，四个方向全部符合预期：

```
old->plain    : FAIL     追加普通字段后，旧数据新端读不了
old->compat   : OK       追加 compatible 后，旧数据新端能读
compat->old   : OK       新数据旧端也能读（双向兼容）
plain->old    : FAIL     追加普通字段后，新数据旧端读不了
```

旧数据经 compatible 版本读出时，该字段为 `unset`（不是空串），这正是加固项统一 unset
语义所要对齐的状态。结论不再是推理。

| 文件 | 字段 | 访问器 | 过线方式 |
| --- | --- | --- | --- |
| `include/types.h` | `Segment::rack_id` | `RackId()` | MountSegment RPC |
| `include/allocator.h` | `Descriptor::rack_id_` | `rack_id()` | GetReplicaList 响应 |
| `include/replica.h` | `ReplicateConfig::rack_id` / `strict_rack` | `RackId()` / `IsStrictRack()` | coro_rpc 参数（无 `YLT_REFL`，struct_pack 自动反射聚合成员） |

全部改为 `struct_pack::compatible<T, 1>`，与仓库既有约定
（`metadata_store.h:39,102-104`）一致。调用点跟随改为走访问器。

附带修正：`Segment::operator==` **显式展开**。

> **已实测修正（2026-09-21）**：此前我基于 harness stand-in 判断"defaulted
> `operator==` 被隐式删除"，**这是错的**。真实的
> `struct_pack::compatible`（`compatible.hpp:133`）除继承 `std::optional<T>` 外还提供
> 了自由函数 `operator==`（`:149`），所以 defaulted 版本编译得过。我的 stand-in 少了
> 这个自由函数，才得出删除的结论。
>
> 但显式展开仍然必要，理由换成实测的语义差异（`g++ -std=c++20` + 真实 ylt 头文件）：
>
> ```
> defaulted: unset == engaged-empty ? false
> explicit : unset == engaged-empty ? true
> ```
>
> `RackId()` 把 unset 与 engaged-empty 都读作空串，因此显式版本判为相等；defaulted
> 版本按 optional 语义认为二者不同。跨版本对接时旧端发 unset、新端发 engaged-empty，
> defaulted 会误判不等。这也正是加固项里统一 unset 语义要解决的同一个问题。

### P0-4（中）无 fabric 支持的机器快速失败

`allocatePinnedLocalMemory` 在 `!supportFabricMem()` 时回落 `cudaMalloc`，对齐不满足
只打 WARNING 就返回指针。但该指针会流到 `MountSegment`，那里硬检查
`buffer % Slab::kSize != 0` 直接 `INVALID_PARAMS`。`cudaMalloc` 不可能保证 16 MB
对齐，所以这条"回落"实际是**先警告、后在 mount 处失败**，中间状态最难排查。

`src/transport/nvlink_transport/nvlink_transport.cpp`：对齐不满足时 `cudaFree` + 返回
`nullptr`，在正确的位置失败。

### P0-5（轻）CXL 不进机架索引

`src/segment.cpp` 的 `AddRackSegment` 增加 `protocol == "cxl"` 早退。CXL 共享一个全局
allocator，无法承载 per-segment 的 rack；给它建索引会让 master 认为同机架、reader 看到
空 rack 当远端，两侧不一致。

同时注明：`protocol` 字段未序列化（既有问题），恢复路径上为空串，但这不构成漏洞 ——
CXL allocator 不是 `OffsetBufferAllocator`，序列化时 `has_buffer_allocator=false`，恢复
路径的非空 `buf_allocator` 检查已经跳过它。

### 加固

| 项 | 改动 | 理由 |
| --- | --- | --- |
| `include/replica_selection.h` | 去掉 `SelectBestReplica` 第三参数的 `= ""` 默认值 | 默认参数是陷阱：以后新增调用点漏传会**静默丢掉机架亲和性**，无任何提示。现在漏传是编译错误 |
| `src/real_client.cpp` | 同步去掉 `SelectCompleteMemoryReplica` / `SelectSessionReplica` 的默认值 | 同上 |
| `src/allocator.cpp` | `get_descriptor()` 仅在 rack 非空时 engage optional | 让无机架 segment 发送真正的 unset 而非 engaged-but-empty |
| `src/client_service.cpp` | `MountSegmentAndGetId()` 同上 | 与"未配置 rack 的旧版本对端发什么"保持一致 |

unset 语义统一后，两者通过 `RackId()` 读都是空，不影响正确性，只是语义更干净。

### 测试

| 文件 | 改动 |
| --- | --- |
| `tests/serializer_test.cpp` | 新增 4 例：带 rack round-trip / 无 rack round-trip / 9 字段旧格式仍可读 / 显式空值 |
| `tests/replica_selection_test.cpp` | 17 个调用点显式传 `""`；`MakeMemory` 改为仅非空时赋 rack，让 unset 路径真的被覆盖 |
| `tests/segment_test.cpp` | `RackIdSurvivesSegmentSnapshotRestore` —— 覆盖 P0-1 后果最重的那半条链路，三点断言：segment 记录 round-trip 带 rack、master 侧 `GetRackSegmentNames` 索引重建、**恢复出来的 allocator 带 rack**（第三点正是原缺陷所在：索引对了但 allocator 空，master 认为同机架而 reader 全看成远端） |

## 3. 分支上原有的改动（我 review 过，非本轮所加）

`git diff --stat` 共 29 个文件、+1142/-74。上面第 2 节是本轮所加；其余属于分支原有的
机架感知 + VRAM fabric 工作：

- `src/config/vram_fabric_config.{h,cpp}` + `tests/vram_fabric_config_test.cpp`（新文件）
- `src/common/client_buffer_allocation.cpp`（+93）、`include/gpu_vendor/mnnvl.h`、
  `include/transport/nvlink_transport/nvlink_transport.h`
- `mooncake-common/include/environment_variables.h`、`include/client_service.h`、
  `include/segment.h`、`include/real_client.h`
- Python 侧：`python/mooncake/mooncake_config.py`、`mooncake-wheel/mooncake/mooncake_store_service.py`
  及各自单测
- `tests/segment_test.cpp`（+139）、`tests/allocation_strategy_test.cpp`、两处 CMakeLists

**归属切分（已用会话起始的 `git status` 快照复核）：**

本轮**新碰**的文件共 3 个 —— 会话开始时它们不在 modified 列表里，所以其全部改动都是
本轮所加：

- `src/serialize/serializer.cpp`（P0-1）
- `src/ha/snapshot/store_resource_snapshot_codec.cpp`（P0-1）
- `tests/serializer_test.cpp`（4 个新用例）

其余 **26 个文件在我介入前就已被分支改动**，属于混合状态。本轮在其中的改动见第 2 节
各表点名的文件与行号；同一文件里的其他 hunk 属于分支原有工作。

未追踪文件（`src/config/vram_fabric_config.{h,cpp}`、`tests/vram_fabric_config_test.cpp`）
均为分支原有，非本轮所加。

提 PR 前仍需按 `AGENTS.md` 要求逐行 review `git diff` —— 上面的切分只到文件粒度，
不替代逐 hunk 审阅。

这部分 review 结论见 `nvlink-rack-affinity-review.md` 第 5.8 节（做对了的部分）与
第 4 节（选路层为零，两条目标需求均未达成）。

## 4. 验证状态

**已编译并跑通（2026-09-21）。** 此前"本机无法构建"的判断是错的，见本节末。

### 构建结果

CMake 配置通过（`-DBUILD_UNIT_TESTS=ON -DWITH_STORE=ON -DWITH_TE=ON
-DWITH_STORE_RUST=OFF -DUSE_CUDA=OFF`），四个受影响目标编译**零 error**：

| 测试 | 结果 |
| --- | --- |
| `serializer_test` | 10/10 通过（含本轮新增 4 例） |
| `replica_selection_test` | 27 通过 / 1 跳过（`EnvironmentOptInUsesBuiltinScorer`，环境相关，非本轮改动） |
| `segment_test` | 24/24 通过（含机架相关 3 例） |
| `vram_fabric_config_test` | 6/6 通过 |

合计 **68 个用例，零失败**。

### 变异验证：测试确实在检验东西

绿灯本身不说明测试有效。把 P0-1 的恢复路径修复临时短路掉
（`it->second.buf_allocator->SetRackId(...)` 前加 `if (false)`）重新编译后：

```
segment_test.cpp:1248: Failure
Expected equality of these values:
    Which is: ""
[  FAILED  ] SegmentTest.RackIdSurvivesSegmentSnapshotRestore
```

报的正是"恢复出来的 allocator 没有 rack"——**原缺陷的准确症状**。改动已还原，复跑
3 个机架用例全绿。这条链路（P0-1 后半段）现在有了真实的回归保护。

### 早期 harness 的一处错误结论（已订正）

改动初期没有编译环境，我用一个 stand-in 模拟 `compatible<T,N>` 跑了 13 项断言。其中
一条结论是错的：**"派生自 `std::optional` 使 defaulted `operator==` 被隐式删除"**。
真实实现额外提供了自由函数 `operator==`（`compatible.hpp:149`），defaulted 版本编译
得过；我的 stand-in 少了它。

显式展开 `Segment::operator==` 仍然必要，但理由是实测的语义差异，见第 2 节 P0-3。
教训：stand-in 的结论只在 stand-in 与真实实现一致时成立，不能当作对真实代码的判断。

### 仍未验证的部分

- **P0-4 在本机编译不到。** `nvlink_transport` 的编译条件是
  `USE_MNNVL AND NOT USE_HIP`，本机无 CUDA（无 `nvcc`），该文件不参与构建。需在有
  CUDA 的环境验证。
- 未跑全量测试套件，只跑了本轮改动直接影响的 4 个目标。
- 未跑 `pre-commit`（工具链不在 PATH）。

~~**P0-3 的类型哈希结论是推理，不是实测。**~~ **已于 2026-09-21 实测确认**，见第 2 节
P0-3。四个方向的跨版本对撞结果均符合预期。

详见上面「构建结果」。

## 5. whl 构建：本机不可行

**未能构建。** 阻塞在依赖，不在代码。

工具失效前确认的事实：

| 检查项 | 结果 |
| --- | --- |
| `cmake` / `gcc` / `g++` / `python3` / `pip3` | 都在 PATH |
| `glog/logging.h` | MISSING |
| `boost/version.hpp` | MISSING |
| `ylt/coro_rpc/coro_rpc_client.hpp` | MISSING |
| `tl/expected.hpp` | MISSING |
| `apt-get install -s libglog-dev` | `E: Unable to locate package libglog-dev` |
| `dependencies.sh` | 基础包走 apt（`:136`、`:186`），绕不过上一行 |
| `scripts/build_wheel.sh` | 直接从 `${BUILD_DIR}` 拷 `.so`，前提是 C++ 已编译成功 |

网络可达性**未能测出** —— 探测命令发出时 Bash 工具已开始返回空输出，无法据此判断。
这是唯一的变数：若 pypi/github 可达，从源码装 glog + boost + ylt + tl::expected 理论
上可行，但 boost 体量大，耗时会很长。

在有依赖的环境里，构建序列是：

```bash
# 1. 装依赖（需要 apt 源可用）
bash dependencies.sh

# 2. 配置 + 编译
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

# 3. 打 wheel（BUILD_DIR 需与上面一致）
bash scripts/build_wheel.sh 3.10 dist

# 4. 跑本轮新增/受影响的单测
./build/mooncake-store/tests/serializer_test
./build/mooncake-store/tests/replica_selection_test
./build/mooncake-store/tests/segment_test
./build/mooncake-store/tests/vram_fabric_config_test
./build/mooncake-store/tests/allocation_strategy_test

# 5. PR 前
pre-commit run --files <本次改动的文件列表>   # 不要用 --all-files
```

产物预期落在 `dist/`，形如 `mooncake_transfer_engine-0.3.13-*.whl`。

## 6. 剩余工作

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| 0 | 本文第 2 节各项 | 已落盘，**待编译验证** |
| 1 | 选路层：nvlink 进 `protocol_priority()` + 机架可达性门控（按决策 #2 走配置驱动，含第 1 节列的三个前置） | 未做 |
| 2 | 双 transport 并存（打破排他 if/else）+ 双协议注册 | 未做 —— 但按决策 #2 它从"优化"升级为**必需**，见第 1 节第 2 项 |
| 3 | 真机（NVL72）验证 | 未做 |
| 4 | 路线 B：EGM，host DRAM 也走 NVLink | 未定，见待决策 #5 |

阶段 0 优先于一切 NVLink 工作 —— 它修的是**已经声称可上线的部分**。

注意当前"阶段 2 先于阶段 1 落地"（fabric 分配已实现、门控未实现），所以
`MC_STORE_VRAM_FABRIC` 目前**只应在隔离节点上验证**。

### 仍待决策

| # | 问题 | 影响 |
| --- | --- | --- |
| 3 | 本地 NOF_SSD vs 同机架 MEMORY 的优先级 | 当前 SSD 优先（沿用历史行为）。GB300 上同机架 NVLink DRAM 约 900 GB/s 量级 vs 本地 NVMe 约 10 GB/s 量级，可能不合理 |
| 4 | 是否做 rack 单一数据源改造（review 文档 7.1） | `rack_id` 现存两处靠手工同步，P0-1 后半段与 P0-5 都是这个重复的症状 |
| 5 | 是否投入路线 B（EGM） | 建议等选路层跑通、真机验证后再定。路线 A 剩余工作量全在选路层，而选路层是两条路共用的前置 |
| 6 | drain / 动态复制是否需要遵守机架约束 | 见下 |

### 决策 #6 的背景（新发现，review 文档 5.5）

滚动升级的数据面原语**不认识 rack_id**。`SelectDrainTargetForKey`
（`master_service.cpp:13793`）选迁移目标只看「非源 segment / 不与现有副本重合 /
allocatable / 有容量」，打分只用 `used / capacity`，全程不看机架。

后果：一次 store 节点滚动升级后，原本以 `strict_rack` 写入、保证全部副本在 rack0 的
key，副本可能落到 rack1。数据仍可读（不是正确性 bug），但 Put 时的放置保证被**事后
作废**，读取侧同机架层随之失效退回 RDMA —— 正是本项目要消除的情况，且全程无日志。

根因：master 不保留原始 `ReplicateConfig`。`ObjectMetadata`（`object_metadata.h:43`）
没有 rack_id 也没有 strict_rack，drain 想遵守也无从得知。

同类缺口：`ReplicaActionProposal` 已有 `requester_domain` / `target_domain`
（`rpc_types.h:20-34`），但 `SelectDynamicReplicaPlan`（`:9207`）收下 `target_domain`
后**从不用它筛候选**，只在 `:9349` 记进 plan、`:9410` 做提案去重。"domain"概念已铺到
RPC 层但没接放置层，而它正是 rack_id 最自然的落点。

建议先做第一档：`SelectDrainTargetForKey` 增加"优先同 rack 候选，无同 rack 时 WARNING
再跨机架"。不需要持久化任何东西 —— 用现有副本所在 segment 的 rack 当参照即可。
