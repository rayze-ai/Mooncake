# TODO

对应 `design.md`。按依赖顺序排,前一阶段的结论决定后一阶段做不做。

## 进度(2026-09-23)

| 阶段 | 状态 | 说明 |
| --- | --- | --- |
| 0 基线 | 跳过 | 后续补。当前只有推断("KV 走 RoCE"),没有计数器数据 |
| 1 PD 直传 NVLink | **待你侧执行** | 两个 env,步骤在 `deployment.md` 一 |
| 2 KV cache fabric | **已定案,待你侧执行** | `--enable-cumem-allocator`,硬件已核准 |
| 3 Mooncake 代码 | **完成** | commit `9f23de2b`,115 上编过,31 例测试通过 |
| 3.5 出 wheel | 待做 | 需 arm64 机器,115 是 x86 |
| 4 部署 | 待做 | 依赖 3.5 |
| 5 验证 store 路径 | 待做 | 依赖 4 |
| 6 可选 | — | |

分支 `yaochen/nvlink`,2 个 commit,**未 push**。

## 阶段 0:先证明现状(不改任何东西)

目的:坐实"当前 KV 走 RoCE",拿到基线。不做这步,后面所有"变快了"都没有对照。

- [ ] 在一个 decode 节点上快照 `mlx5_bond_*/ports/1/hw_counters/rx_write_requests` 和 `nvidia-smi nvlink -gt d`
- [ ] 打一批触发 PD 交接的请求,再快照
- [ ] 预期:RDMA 计数器涨 → 确认 PD 直传走 RoCE
- [ ] 在 P pod 里 `strings nixl_cu13.libs/ucx/libucs.so* | grep -E '^1\.[0-9]+\.[0-9]+'` 拿 UCX 版本号,记下来
- [ ] 拿到 `mc_store_rest_server` 的 pod spec、启动 env、wheel 版本(`MOONCAKE_VERSION` 对应哪个 commit)

## 阶段 1:PD 直传走 NVLink(只改 env,不改代码)

- [ ] P、D 两侧加 `UCX_CUDA_IPC_ENABLE_MNNVL=y`
- [ ] P、D 两侧加 `UCX_LOG_LEVEL=info`(验证期)
- [ ] 重启,看日志:
  - [ ] 有 `fabric_info: state=3 ... uuid=74a91164...`
  - [ ] **有没有** `does not have fabric property` ← 有则说明 KV cache 不是 fabric 内存,进阶段 2
- [ ] 重做阶段 0 的计数器差分,预期 RDMA 不涨、NVLink Rx 涨
- [ ] 通了:去掉 `UCX_LOG_LEVEL`,记录前后延迟对比

**这一步不依赖 Mooncake 任何改动,一天内能出结果。**

## 阶段 2:vLLM KV cache 带 fabric handle(一个 CLI 参数)

**结论**:P、D 各加 `--enable-cumem-allocator`。零代码。

依据(`/data/code/vllm`,v0.29.0):

- `gpu_worker.py:734`:KV cache 分配包在 `use_memory_pool(tag="kv_cache")` 里
- `cumem.py:77-78`:`torch.cuda.memory.use_mem_pool(MemPool(CUDAPluggableAllocator))`,
  上下文内所有分配(含 `utils.py:411` 的 `torch.zeros`)进这个 allocator
- `csrc/cumem_allocator.cpp:143-148`:设备 `FABRIC_SUPPORTED` 为真 →
  `requestedHandleTypes = CU_MEM_HANDLE_TYPE_FABRIC`
- `model.py:346`:`enable_cumem_allocator` 默认 False;`--enable-sleep-mode` 会自动带上,但不需要

**否掉的两条路**:

| 原方案 | 为什么不行 |
| --- | --- |
| `PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True` | vLLM **主动拒绝**(`config/vllm.py:1000-1041`):配了 KV connector 就 `ValueError`。VMM 会在引擎生命周期内把 VA 重映射到不同物理页,pin 住的注册指向过期页 |
| 自写 `.so` + patch vLLM(参考部署那套) | 与 cumem **同一套机制、同一个 PyTorch API**。上游已内置,重复造轮子,且多了版本绑定的锚点 |

- [ ] P、D 各加 `--enable-cumem-allocator`
- [x] 硬件已核准(host-10-0-3-71):`cuMemCreate(DEVICE, FABRIC)` rc=0、`cuMemExportToShareableHandle` rc=0、`nvidia-smi -q` Fabric `State: Completed` `CliqueId 32766`
- [ ] 重启后看 UCX 日志**无** `does not have fabric property` —— 它现在多一个含义:cumem 在 `:157-160` 静默回落到了 POSIX FD
- [ ] 计数器差分同阶段 1

**一个注意**:cumem 的 FABRIC 失败回落是静默的(`cumem_allocator.cpp:157-160`),分配照样成功,
UCX/TE 那边才发现导不出。所以阶段 1 那条日志仍是必看项。

## 阶段 3:Mooncake 改代码(fabric 那一半)—— 完成

原前置是"阶段 1 通了再做"。实际按你的决定并行做了:代码本身不依赖阶段 1 的结果,
依赖的只是部署价值。**部署价值仍然取决于阶段 1**:阶段 1 不通,store 这条路即便改好,
vLLM KV cache 注册进 TE 也是空的。

### 3.1 host DRAM VMM 分配器

- [x] `mooncake-store/src/common/host_fabric_allocator.{h,cpp}`
  - [x] `cuMemCreate(HOST_NUMA, id=cudaDevAttrHostNumaId, FABRIC)`
  - [x] access desc `device_count + 1`(每 GPU + host NUMA)
  - [x] 对齐 `max(granularity, Slab::kSize)`
  - [x] 释放:先 `cuMemRetainAllocationHandle` 再 unmap;size 用 `cuMemGetAddressRange` 回取
  - [x] 任一步失败 → 清理 + 返回 `nullptr`,**不回落 `aligned_alloc`**
  - [x] `cudaDevAttrHostNumaId` 查不到 → 失败,**不猜 node 0**
- [x] `mooncake-store/src/config/host_fabric_config.{h,cpp}`:`MC_STORE_HOST_FABRIC` 取值判定
- [x] `src/CMakeLists.txt`:`USE_CUDA` 分支补 `CUDA::cuda_driver`

### 3.2 谓词不看 protocol

- [x] `TransferEngine::nvlinkUsesFabricMem()` → `Impl` → `MultiTransport::nvlinkUsesFabricMem()`(新增,`dynamic_cast` 到 `NvlinkTransport::usesFabricMem()`);TENT 分支返回 false
- [x] `Client::NvlinkUsesFabricMem()`
- [x] `client_buffer_allocation.{h,cpp}`:进程级标志 `set_nvlink_fabric_ready()` / `nvlink_fabric_ready()`
- [x] `real_client.cpp` setup:`Client::Create` 之后、首次分配之前 `set_nvlink_fabric_ready(client_->NvlinkUsesFabricMem())`
- [x] 谓词:`if (!nvlink_fabric_ready()) return false; return HostFabricConfig::enabled();`
- [x] `free_memory()` 分支**逐项镜像**分配端

### 3.3 静默失败改硬错(按 `remote_accessible` 区分,默认不容忍)

原计划"一律 `return -1`"**不能做**:那条 `return 0` 有合法用户 —— Store 的 `local_buffer`
(`real_client.cpp:962`,`aligned_alloc`)只做本地暂存,从不给远端读。一律硬错会让
`standalone-store` 下 P/D 的 vLLM 进程(只有 local_buffer,无 segment)**启动直接失败**。

`registerLocalMemory` 的 `remote_accessible` 参数正好区分这两种意图,且两个调用方已经
传对了值(MountSegment 传 `true`,local_buffer 传 `false`),只是 nvlink 分支原来
`(void)` 掉了:

| `remote_accessible` | 非 VMM 内存时 |
| --- | --- |
| `false` | INFO 日志,`return 0` —— 本地暂存,跳过发布是正确结果 |
| `true` + `MC_NVLINK_TOLERATE_NON_FABRIC` 已设 | WARNING,`return 0` —— 用户明确接受不可达 |
| `true`(默认) | **ERROR,`return -1`** → MountSegment 拒绝(`client_service.cpp:3825` 返 `INVALID_PARAMS`),segment 不进 master 索引 |

- [x] `nvlink_transport.cpp` fabric 分支:按上表三路
- [x] 确认 MountSegment 对 -1 的处理:`INVALID_PARAMS`,不吞
- [x] IPC 分支(`:983`)的 `(void)remote_accessible` 保留 —— 那条路 `cudaIpcGetMemHandle` 失败本来就 `return -1`
- [x] `MC_NVLINK_TOLERATE_NON_FABRIC` 是存在即生效 —— TE 侧 env 惯例(同 `MC_USE_NVLINK_IPC`),且它是"放宽"开关,误设的后果是恢复旧行为,不是新风险

### 3.4 编译与测试

- [x] `cmake .. -DUSE_CUDA=ON -DUSE_MNNVL=ON -DBUILD_UNIT_TESTS=ON` —— **`USE_MNNVL` 不能省**,默认 OFF,省了 nvlink transport 不编入。**不要加 `-DSKBUILD=ON`**:115 上无系统 pybind11,它会让 `find_package(pybind11)` 失败;走 `extern/pybind11` 子模块即可
- [x] `host_fabric_config_test`:6 例(取值判定、无效值、大小写)—— **通过**(115 机器)
- [x] `nvlink_fabric_ready_test`:4 例(默认 false、round-trip、last-write-wins、开关后分配仍工作)—— **通过**(115 机器)
- [x] `buffer_allocator_test` 不被破坏
- [x] 需 folly,本地编不了 → 在 CUDA 机器上编

**远端已验证**(115,`-DUSE_CUDA=ON -DUSE_MNNVL=ON`):全部 20 个改动文件过编译器;
三个测试 6 + 4 + 21 全过;pre-commit 全过(cmake-format 对 `dynamic_replication_lease_table_test`
那行的重排按 AGENTS.md 留在 PR 外)。commit `9f23de2b`。

编译时暴露并修掉的两处:`usesFabricMem()` 最初插进了 `protected:` 段;
`host_fabric_config.h` 最初放在 `src/config/`,测试的 include 路径不含它,按惯例挪到 `include/config/`。

### 3.5 出 wheel

- [ ] 找 arm64 构建机 —— 115(`host-192-168-172-115`)是 x86,编出来的 wheel GB300 用不了。候选:GB300 节点本身,或 arm64 CI runner
- [ ] 在那台机器上重跑 3.4 的 cmake + make,确认 arm64 下也能编(x86 过了不代表 arm64 过)
- [ ] `OUTPUT_DIR=dist ./scripts/build_wheel.sh`
- [ ] 推到 `vllm-code-server.ruizi-k3pd:9099/mooncake.whl`,更新 `MOONCAKE_VERSION`

## 阶段 4:部署配置

### store 进程(`mc_store_rest_server`)

- [ ] 换新 wheel
- [ ] env:`MC_STORE_HOST_FABRIC=1`、`MC_FORCE_MNNVL=1`
- [ ] json:`protocol=rdma`(**不是** `nvlink`)、`global_segment_size` 按容量
- [ ] 非 privileged 则加 `NVIDIA_IMEX_CHANNELS=0`;privileged 不需要
- [ ] 确认 `MC_STORE_USE_HUGEPAGE` **未设**(存在即生效,会截走 fabric 路径)

### Prefill

- [ ] 换新 wheel
- [ ] env 加 `MC_FORCE_MNNVL=1`(让 P 的 TE 装 nvlink 而非 rdma)
- [ ] connector 不动(已是 `MultiConnector` + Nixl producer + Store kv_both)

### Decode

- [ ] 换新 wheel
- [ ] `--kv-transfer-config` 改成 `MultiConnector`,加 `MooncakeStoreConnector`(`kv_consumer`,`enable_lookup: true`)
- [ ] `--no-enable-prefix-caching` → `--enable-prefix-caching`(否则 lookup 无块可匹配)
- [ ] env 加 `MC_FORCE_MNNVL=1`、`MOONCAKE_CONFIG_PATH`,json 同 P(`standalone-store`,`global_segment_size=0`)

## 阶段 5:验证 store 路径

- [ ] store 进程日志两条都有:`Using NVLink transport` + `Allocated ... fabric host memory`
- [ ] P、D 进程日志:`Using NVLink transport`(不是 `Using RDMA transport`)
- [ ] 打两轮同前缀请求,第二轮 D 侧日志有 store 命中、**无** miss
- [ ] 命中期间计数器差分:RDMA 不涨、NVLink Rx 涨
- [ ] 对照:store 关 `MC_STORE_HOST_FABRIC` 重启一次,同样的请求应看到 RDMA 涨 —— 证明差异确实来自 fabric

## 阶段 6(可选,通了再说)

- [ ] D 加 `save_decode_cache: true`,多轮对话下一轮能命中上一轮的 decode 输出
- [ ] `MC_TE_METRIC=1` 看延迟分布
- [ ] 评估 TP8 下 P 侧 8 个 rank × 4GB local_buffer 是否需要收

## 明确不做

- 跨机架、拓扑感知、rack_id
- `MC_STORE_VRAM_FABRIC`(store 是 DRAM 池)
- 用 `MooncakeConnector` 替换 `NixlConnector`
- `pre-commit run --all-files`
