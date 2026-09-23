# 改动清单

分支 `yaochen/nvlink`,从 `main`(`31dedebb`)切出。代码 1 个 commit,其余为文档:

| commit | 内容 |
| --- | --- |
| `9f23de2b` | `[TE][Store] Allocate host segments as fabric memory when nvlink runs in fabric mode` |
| `5225f57c` `ac6f20ad` `ee02bd6a` | `ppio/` 文档 |

20 个文件,+637 −7。**447 行在 6 个新文件里**,原有 14 个文件合计 +190,单文件最多 54 行
(`client_buffer_allocation.cpp`),其余都是 5-16 行的接口透传。

---

## 一、要解决的问题

`NvlinkTransport::registerLocalMemory()`(`nvlink_transport.cpp:997`)对注册的内存调
`cuMemRetainAllocationHandle()`。这个调用只对 CUDA VMM(`cuMemCreate`)分配的指针成功;
`aligned_alloc` / `cudaMalloc` 的指针会失败。**失败后 `main` 的行为是打一条 WARNING 然后
`return 0`** —— 注册"成功"但什么都没发布:

```
aligned_alloc 的 segment
  → registerLocalMemory 静默注册了空
  → MountSegment 成功
  → segment 出现在 master 索引里
  → 远端选路选中它
  → 第一次读 miss
```

失败点离原因隔五步。而 `main` 上 Store 没有任何路径把 host segment 分配成 VMM 内存
(唯一的 fabric 分配是 `nvlink_intra` 协议的 `allocateFabricMemory_intra`,同机用),所以
NVL72 上 store 这条路**根本走不了 NVLink**。

三处改动对应三件事:让 segment 分配成 VMM(§2);让分配器知道该不该这么分(§3);
让上面那条静默路径在该报错的时候报错(§4)。

---

## 二、host DRAM 的 VMM 分配器(新文件)

| 文件 | 行数 |
| --- | --- |
| `mooncake-store/include/common/host_fabric_allocator.h` | 36 |
| `mooncake-store/src/common/host_fabric_allocator.cpp` | 227 |
| `mooncake-store/include/config/host_fabric_config.h` | 17 |
| `mooncake-store/src/config/host_fabric_config.cpp` | 37 |

两个函数:

```cpp
void *allocate_fabric_host_memory(size_t size, size_t alignment);
void  free_fabric_host_memory(void *ptr);
```

`allocate` 的调用序列:

```
cudaDeviceGetAttribute(cudaDevAttrHostNumaId)        ← 取当前 device 的 host NUMA node
cuMemGetAllocationGranularity(HOST_NUMA, FABRIC)
cuMemCreate(padded_size, {HOST_NUMA, node, FABRIC})
cuMemAddressReserve(padded_size, alignment)
cuMemMap
cuMemSetAccess(device_count + 1 个 descriptor)       ← 每个 GPU 一个 + host NUMA 一个
```

几个决定:

- **access descriptor 是 `device_count + 1`**,不是 `device_count`。这块内存由 CPU 写
  (Store 往里 memcpy)、GPU 读,两种 location type 都要授权。VRAM segment 不需要 host 条目。
- **对齐取 `max(granularity, alignment)`**(两者都是 2 的幂时)。Store 把基址交给 cachelib,
  要 `Slab::kSize`(16MB)对齐,`MountSegment` 拒绝未对齐的。
- **NUMA node 查不到就失败**,不猜 node 0。猜错了照样"能用",只是每个 segment 落在错的
  socket 上,下游没有任何东西会报。
- **任一步失败:清理已分配的、返回 `nullptr`,不回落 `aligned_alloc`。** 回落的指针只会把
  失败推到 §1 那条链的末端,错误信息更难懂。
- **handle 生命周期对齐 `NvlinkTransport::allocatePinnedLocalMemory` / `freePinnedLocalMemory`**:
  分配后不 release,free 时 `cuMemRetainAllocationHandle` 取回、`cuMemGetAddressRange` 回取
  size(不用全局 map 记尺寸)、unmap、release 一次。第一版写成了 release 两次,对照 TE
  的实现后改掉。

`HostFabricConfig` 读 `MC_STORE_HOST_FABRIC`,**取值判定**:`=0` 是关,`maybe` 之类无效值
告警并保持关。和 `MC_STORE_USE_HUGEPAGE` 的存在即生效相反 —— 这里静默 opt-in 选中的
失效模式正是 §1 那条链,不能允许。

`environment_variables.h` +7:声明 `HostFabricEnvironmentVariables::MC_STORE_HOST_FABRIC`。

---

## 三、分配谓词:问 TE 实际装了什么,不看 protocol 字符串

### 3.1 为什么不能看 protocol

nvlink transport 装不装,由 `transfer_engine_impl.cpp:381-425` 决定:编译期 `USE_MNNVL` /
`USE_INTRA_NVLINK`,运行期 `MC_FORCE_MNNVL` / `MC_INTRANODE_NVLINK` / 无 HCA。
**`transfer_engine_->init()` 不接收 protocol 参数**。而 Store 侧:

- `real_client.cpp` 全文没有 `"nvlink"` 字面量
- `IsHostStoreSegmentProtocol()` 白名单是 `"" / tcp / rdma / efa / cxi / rpc_only`,写
  `protocol=nvlink` 会让 segment 丢掉 `cudaHostRegister`

所以常规配置是 `protocol=rdma` + `MC_FORCE_MNNVL`。谓词必须能在 `protocol=rdma` 下判出
"这个进程的 TE 在导出 fabric handle"。

### 3.2 四层透传(TE 侧,各 5-12 行)

| 层 | 文件 | 新增 |
| --- | --- | --- |
| `NvlinkTransport` | `nvlink_transport.h:116` | `bool usesFabricMem() const` —— 暴露已有的 `use_fabric_mem_`。**必须在 `public:` 段**,第一版插进了 `protected:`,`dynamic_cast` 后调用编不过 |
| `MultiTransport` | `multi_transport.h` +7,`multi_transport.cpp:783` +11 | `nvlinkUsesFabricMem()`:`transport_map_.find("nvlink")` → `dynamic_cast<NvlinkTransport*>` → `usesFabricMem()`。`#ifdef USE_MNNVL` 保护,否则恒 false |
| `TransferEngineImpl` | `transfer_engine_impl.h` +5 | pimpl 中间层,`multi_transports_` 为空时 false |
| `TransferEngine` | `transfer_engine.h` +7,`transfer_engine.cpp:289, :960` +12 | **两个实现**,因为 `getTransport` 有两个重载(`:285` 普通,`:949` 带 `use_tent_`)。TENT 分支返回 false —— TENT 自己管 transport,不装 nvlink |

`use_fabric_mem_` 是 `NvlinkTransport` 的实例属性,install 时由设备能力 + `MC_USE_NVLINK_IPC`
一次决定。所以它是**节点属性**,进程生命周期内不变。

### 3.3 Store 侧(+87)

| 文件 | 新增 | 内容 |
| --- | --- | --- |
| `client_service.h:662` | +8 | `Client::NvlinkUsesFabricMem()`,转发到 `transfer_engine_->nvlinkUsesFabricMem()` |
| `client_buffer_allocation.h` | +16 | `set_nvlink_fabric_ready(bool)` / `nvlink_fabric_ready()` 声明 |
| `client_buffer_allocation.cpp` | +54 | 见下 |
| `real_client.cpp:960` | +9 | setup 里 `Client::Create` 之后、首次分配之前:`set_nvlink_fabric_ready(client_->NvlinkUsesFabricMem())` |

`client_buffer_allocation.cpp` 的 54 行:

```cpp
#if defined(USE_CUDA) && !defined(USE_VRAM_SEGMENT)
#define MOONCAKE_STORE_FABRIC_HOST 1      // :7
```

编译条件排除 `USE_VRAM_SEGMENT`:host fabric 分支在 `allocate_buffer_allocator_memory()`
末尾 `aligned_alloc` 之前,而 `USE_VRAM_SEGMENT` 构建在前面就 `return` 了,编进去是死代码。

```cpp
bool g_nvlink_fabric_ready = false;       // 进程级标志

bool use_fabric_host() {                  // :48
    if (!nvlink_fabric_ready()) return false;
    static const bool enabled = HostFabricConfig::IsEnabledFromEnvironment();
    return enabled;
}
```

分配端和释放端各一个分支,**逐项镜像**:

```cpp
// allocate_buffer_allocator_memory() 末尾
if (use_fabric_host()) {
    void *ptr = allocate_fabric_host_memory(total_size, alignment);
    if (ptr == nullptr) LOG(ERROR) << "... unset MC_STORE_HOST_FABRIC to use plain host memory";
    return ptr;                           // 不回落
}
return aligned_alloc(alignment, total_size);

// free_memory() 末尾
if (use_fabric_host()) { free_fabric_host_memory(ptr); return; }
free(ptr);
```

**为什么用进程级标志而不是给谓词穿参数**:分配端和释放端必须读到同一个值。穿参数的话
`free_memory()` 那个镜像分支也得收到,漏一个就会对 VMM 映射调 glibc `free()`,在从未分发
过的指针上 abort。标志天然保证两端一致。`static const bool` 缓存 env 也是同样的理由。

---

## 四、`registerLocalMemory` 按 `remote_accessible` 区分(`nvlink_transport.cpp` +44 −7)

### 4.1 原计划为什么不能做

`todo.md` 3.3 原计划是"`cuMemRetainAllocationHandle` 失败一律 `return -1`"。**不行**:
那条 `return 0` 有合法用户 —— Store 的 `local_buffer`(`real_client.cpp:962`,`aligned_alloc`)
只做 Put/Get 的本地暂存,从不给远端读,注册时跳过发布是正确结果。一律硬错会让
`standalone-store` 下 P/D 的 vLLM 进程(只有 local_buffer,无 segment)**启动直接失败**。

### 4.2 现成的信号

`registerLocalMemory(addr, length, location, bool remote_accessible, ...)` 的第四个参数
正好表达调用方意图,且两个 Store 调用方已经传对了值:

| 调用方 | 传的 | 内存 |
| --- | --- | --- |
| `client_service.cpp:3823` MountSegment | `true` | segment,远端要读 |
| `real_client.cpp:980` local_buffer | `false` | 本地暂存 |

只是 nvlink 分支原来 `(void)remote_accessible;` 丢掉了它。

### 4.3 三路判定(`:1012` 附近)

```
cuMemRetainAllocationHandle 失败(非 VMM 内存)时:
  remote_accessible == false           → INFO,  return 0    本地暂存,正确结果
  remote_accessible == true
    && MC_NVLINK_TOLERATE_NON_FABRIC   → WARNING, return 0  用户明确接受不可达
    && 未设(默认)                       → ERROR,  return -1  MountSegment 拒绝
```

`return -1` 的下游已核准:`client_service.cpp:3825` 对 `rc != 0` 返 `INVALID_PARAMS`,
segment 不进 master 索引。**§1 那条五步链在第一步就断了。**

`MC_NVLINK_TOLERATE_NON_FABRIC` 是存在即生效 —— TE 侧 env 惯例(同 `MC_USE_NVLINK_IPC`),
且它是"放宽"开关,误设的后果是恢复旧行为,不是新风险。

IPC 分支(`:983`)的 `(void)remote_accessible` 保留 —— 那条路 `cudaIpcGetMemHandle` 失败
本来就 `return -1`,没有静默问题。

---

## 五、CMake(+10)

| 文件 | 改动 |
| --- | --- |
| `mooncake-store/src/CMakeLists.txt` | `MOONCAKE_STORE_CLIENT_SOURCES` 加两个 `.cpp`;`mooncake_store` 和 `mooncake_client` 的 `USE_CUDA` 分支补 `CUDA::cuda_driver` —— VMM 的 `cuMemCreate` 等符号在 `libcuda` 不在 `cudart` |
| `mooncake-store/tests/CMakeLists.txt` | 注册两个测试 |

cmake-format 想顺手重排 `dynamic_replication_lease_table_test` 那一行(预存的长行),
按 AGENTS.md 留在 PR 外,手工还原了两次。

---

## 六、测试(新文件,130 行)

| 测试 | 用例 | 覆盖 |
| --- | --- | --- |
| `host_fabric_config_test.cpp`(81) | 6 | 未设 / `1` / `TRUE` / `0` / `false` / `maybe`。最后一个验证无效值不会静默 opt-in |
| `nvlink_fabric_ready_test.cpp`(49) | 4 | 默认 false;round-trip;**last-write-wins**(setup 可能跑两次,旧的 true 不能把后来的非 nvlink 节点送进 fabric 分配器);标志设了但 env 关着时分配 + 释放走普通路径 |

`buffer_allocator_test`(21 例,原有)作为回归。

三个测试在 115(x86,CUDA 12.9,`-DUSE_CUDA=ON -DUSE_MNNVL=ON`)上 **6 + 4 + 21 全过**。

---

## 七、环境变量汇总

| 名称 | 位置 | 语义 | 判定 |
| --- | --- | --- | --- |
| `MC_STORE_HOST_FABRIC` | Store env,设在 **mount segment 的进程**(standalone-store 下是 `mc_store_rest_server`) | host DRAM 分配成 VMM + fabric handle | 取值,`=0` 关 |
| `MC_NVLINK_TOLERATE_NON_FABRIC` | TE env | 远端可达的注册遇到非 VMM 内存时容忍(恢复旧行为) | 存在即生效 |
| `MC_FORCE_MNNVL` | TE env(原有) | 装 nvlink transport | 存在即生效 |

---

## 八、编译时暴露并修掉的

| 问题 | 原因 | 修法 |
| --- | --- | --- |
| `usesFabricMem()` protected within this context | 用正则找 `registerLocalMemory` 声明当锚点插 getter,它在 `protected:` 段 | 挪到 `freePinnedLocalMemory` 之后、`protected:` 之前 |
| `config/host_fabric_config.h: No such file` | 放在 `src/config/`,测试的 include 路径只有 `include/` | 挪到 `include/config/`,与 `bucket_backend_config.h` 等测试可见的 config 头一致 |
| `-DSKBUILD=ON` 让 `find_package(pybind11)` 失败 | 115 无系统 pybind11 | 去掉,走 `extern/pybind11` 子模块 |
| `free` 里 `cuMemRelease` 两次 | 对 `cuMemRetainAllocationHandle` 是否增引用理解有误 | 对照 `NvlinkTransport::freePinnedLocalMemory`,改成一次 |

---

## 九、未验证

| 项 | 状态 |
| --- | --- |
| `cuMemCreate(HOST_NUMA + FABRIC)` 真实硬件行为 | **只过编译器**。GB300 上核准的是 `DEVICE` 变体(rc=0),`HOST_NUMA` 变体没试 |
| arm64 构建 | **未做**。115 是 x86,GB300 是 Grace |
| P → store → D 端到端 | 未跑 |
| `buffer_allocator_test` 以外的回归 | 未跑 |
