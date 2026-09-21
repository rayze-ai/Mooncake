# 编译与部署

本文档记录的编译步骤在 `192.168.172.115`(Ubuntu 22.04,CUDA 12.9.86,gcc 11.4.0,128 核)上**实际跑通**。带宽/延迟数字一概不在本文档，见 `change.md` §2.5 的出处说明。

## 1. 两种编译配置

| 配置 | 开关 | 得到什么 |
| --- | --- | --- |
| **A. 只要机架感知** | 无需任何 GPU 开关 | 放置/选择/选路的机架感知。机架感知逻辑**始终编入**,不受开关控制 |
| **B. 加 fabric 分配** | `-DUSE_CUDA=ON`(host fabric)<br>`-DUSE_VRAM_SEGMENT=ON -DUSE_MNNVL=ON`(VRAM fabric) | A 的全部 + fabric handle 分配 |

B 是 A 的超集。先上 A 收益确定且风险低，见 `design.md` §2。

注意 host fabric 与 VRAM fabric 的编译条件是**互斥**的：

```cpp
#if defined(USE_CUDA) && !defined(USE_VRAM_SEGMENT)   // host fabric 才编入
```

原因见 `design.md` §9 —— `USE_VRAM_SEGMENT` 构建走不到 host 分配路径。所以要 host fabric 就**不要**开 `USE_VRAM_SEGMENT`。

## 2. 依赖

以下是在一台干净的 Ubuntu 22.04 上逐个撞出来的完整列表(每一项都是编译失败后才补上的):

```bash
apt-get update
apt-get install -y \
  libyaml-cpp-dev libgoogle-glog-dev libgflags-dev \
  libzstd-dev libxxhash-dev libjsoncpp-dev \
  libcurl4-openssl-dev libssl-dev \
  libibverbs-dev librdmacm-dev libnuma-dev \
  libboost-all-dev libmsgpack-dev \
  pybind11-dev python3-pybind11
```

按撞到的顺序和对应报错：

| 缺什么 | 报错 |
| --- | --- |
| `libyaml-cpp-dev` | `Could not find a package configuration file provided by "yaml-cpp"` |
| `libzstd-dev` | `zstd development files not found` |
| `libxxhash-dev` | `mooncake_provide_xxhash` |
| `libcurl4-openssl-dev` | `Could NOT find CURL (missing: CURL_LIBRARY CURL_INCLUDE_DIR)` |
| `libjsoncpp-dev` | `Target ... links to target "JsonCpp::JsonCpp" but the target was not found` |
| `pybind11-dev` | `Target "rpc_communicator" links to target "pybind11::headers" but the target was not found` |
| `libibverbs-dev` `libnuma-dev` | `infiniband/verbs.h: No such file or directory`,`numa.h`,`cannot find -libverbs` |
| `libboost-all-dev` | `boost/functional/hash.hpp: No such file or directory` |
| `libmsgpack-dev` | `msgpack.hpp: No such file or directory` |

仓库自带 `dependencies.sh`,能用就优先用它。上面的列表是在无法跑该脚本时的最小集。

## 3. pybind11 子模块问题

顶层 `CMakeLists.txt:41-47`：

```cmake
if(SKBUILD)
  set(PYBIND11_FINDPYTHON ON)
  find_package(pybind11 CONFIG REQUIRED)
else()
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/pybind11)
endif()
```

默认走 `extern/pybind11` 子模块。如果 `git submodule update --init --recursive` 因网络起不来(GitHub 连接超时/TLS 中断),用 **`-DSKBUILD=ON`** 走系统 pybind11：

```bash
cmake .. -DUSE_CUDA=ON -DBUILD_UNIT_TESTS=ON -DSKBUILD=ON
```

`SKBUILD` 在本仓库只影响两处：pybind11 的来源，以及是否 `add_subdirectory(python)`。对 Store/TE 的编译产物没有影响。

不要用注释掉 `add_subdirectory(mooncake-integration)` 的办法绕 —— 那会连带缺 `pybind11::headers` 的 `rpc_communicator`,反而更麻烦。

## 4. 完整编译步骤

```bash
cd <repo>
mkdir -p build && cd build

# 配置(host fabric 路线)
cmake .. -DUSE_CUDA=ON -DBUILD_UNIT_TESTS=ON -DSKBUILD=ON

# 或 VRAM fabric 路线
# cmake .. -DUSE_VRAM_SEGMENT=ON -DUSE_MNNVL=ON -DBUILD_UNIT_TESTS=ON -DSKBUILD=ON

# 编译
make -j$(nproc)
```

`USE_VRAM_SEGMENT=ON` 和 `USE_MNNVL=ON` 都会隐式 `set(USE_CUDA ON)`(`mooncake-common/common.cmake:223-239`),不需要额外指定。

### 单独编译某个测试

全量编译耗时较长。只验证 fabric 相关：

```bash
make host_fabric_config_test vram_fabric_config_test buffer_allocator_test -j$(nproc)
```

这会连带编出 `transfer_engine`、`mooncake_store` 等依赖，所以也足以验证新代码的编译正确性。

### 只做语法检查

不想全量编译时，单文件编译最快：

```bash
g++ -std=c++17 -c -I mooncake-common/include -I mooncake-store/include \
    mooncake-store/src/config/host_fabric_config.cpp -o /tmp/hfc.o

g++ -std=c++17 -DUSE_CUDA -c -I mooncake-common/include -I mooncake-store/include \
    -I/usr/local/cuda/include \
    mooncake-store/src/common/host_fabric_allocator.cpp -o /tmp/hfa.o
```

第二条**必须带 `-DUSE_CUDA`**,否则只编到非 CUDA 的桩分支，CUDA 代码一行都不过编译器。这一点是踩过的坑：本地 `USE_CUDA=OFF` 的构建让 `Variables::MC_STORE_HOST_FABRIC` 的声明遗漏一直没暴露。

## 5. 运行测试

```bash
cd build/mooncake-store/tests
./host_fabric_config_test
./vram_fabric_config_test
./buffer_allocator_test
```

实测结果(115 机器，`USE_CUDA=ON`)：

```
host_fabric_config_test   [  PASSED  ] 6 tests.
vram_fabric_config_test   [  PASSED  ] 6 tests.
buffer_allocator_test     [  PASSED  ] 21 tests.
```

`InvalidValueFallsBackDisabled` 会打一条 WARNING,这是预期的(验证无效值不会静默 opt-in)：

```
W host_fabric_config.cpp:27] Invalid MC_STORE_HOST_FABRIC='maybe'.
  Expected a boolean. Fabric host memory stays disabled.
```

或用 ctest：

```bash
cd build && ctest -R "fabric|buffer_allocator" --output-on-failure
```

## 6. 部署配置

### 6.1 机架感知(配置 A)

**每个节点**都要设机架标识。TE 和 Store 各读各的，但 `MOONCAKE_RACK_ID` 两边都认：

```bash
export MOONCAKE_RACK_ID=rack0      # TE 与 Store 都读
# export MC_RACK_ID=rack0          # 仅 TE,优先级高于上面那个
```

Store 侧也可以走配置文件：

```json
{
  "rack_id": "rack0",
  "strict_rack": false
}
```

或 env：

```bash
export MOONCAKE_STRICT_RACK=false
```

`strict_rack` 的取舍见 `design.md` §5:默认 `false`(soft,本机架满了跨机架外溢);设 `true` 则本机架满了分配失败并触发淘汰。

**rack_id 是精确相等比较**,不做大小写归一 —— `Rack0` 和 `rack0` 是两个机架。首尾空白会被 trim,但中间的不会。

k8s 上给节点打标 + 注入 env：

```bash
kubectl label nodes node-1 node-2 node-3 node-4 mooncake.io/rack-id=rack0
```

```yaml
nodeSelector:
  mooncake.io/rack-id: rack0
env:
- name: MOONCAKE_RACK_ID
  value: rack0
```

两处必须一致 —— nodeSelector 决定 pod 落在哪，env 决定 Mooncake 认为自己在哪。不一致会让选路基于错误的拓扑做判断。

### 6.2 fabric 分配(配置 B)

```bash
# host DRAM fabric (EGM) —— 需要 USE_CUDA 且未开 USE_VRAM_SEGMENT 的构建
export MC_STORE_HOST_FABRIC=1

# 或 VRAM fabric —— 需要 USE_VRAM_SEGMENT + USE_MNNVL 的构建
export MC_STORE_VRAM_FABRIC=1
```

两者都是**取值判定**:`=0` 明确是关，无效值(如 `maybe`)告警并保持关闭。

只对 `protocol == "nvlink"` 的 segment 生效。其他 protocol 走 RDMA/TCP,fabric handle 没有意义，谓词会直接返回 false。

GB300 上还需要 IMEX channel：

```yaml
env:
- name: NVIDIA_IMEX_CHANNELS
  value: '0'
```

container toolkit 据此注入 `/dev/nvidia-caps-imex-channels/channel0`,机制与 `/dev/nvidia*` 相同，**不需要 privileged 或手工挂载**。缺它 GB300 上 NCCL MNNVL 直接失败。

### 6.3 vLLM 侧

见 `change.md` §2。要点：

```bash
# 在 vllm serve 之前
bash <path>/kv-fabric/apply_kv_fabric_patch.sh
```

```yaml
env:
- name: VLLM_KV_CACHE_FABRIC_ALLOC
  value: '1'
- name: NVIDIA_IMEX_CHANNELS
  value: '0'
- name: NCCL_MNNVL_ENABLE
  value: '1'
```

patch 脚本幂等，但 vLLM 路径写死为 `/usr/local/lib/python3.12/dist-packages/vllm`,版本不同需要改。脚本用 `assert anchor in src` 卡三个锚点，vLLM 改了这些代码会**直接失败而不是静默不 patch**。

## 7. 上线验证

### 7.1 机架感知生效了吗

TE 选路日志(需要 `VLOG` 级别)：

```
MultiTransport::selectTransport route: target_id=... segment_protocol="nvlink"
  local_ipc_reachable=0 nvlink_reachable=1 chosen=nvlink
```

`nvlink_reachable=1` 且 `chosen=nvlink` 说明同机架判定成功。

跨机架时应看到降级告警：

```
Target segment <name> unreachable over nvlink: target rack "rack1" differs
from local rack "rack0" and an NVLink fabric handle is not importable across
NVLink domains; using rdma
```

Store 放置侧(`VLOG(1)`)：

```
key=..., rack_id=rack0, rack_preferred_segments=4          # soft
key=..., rack_id=rack0, strict_rack_excluded=8, rack_serving_segments=4  # strict
```

### 7.2 fabric 分配生效了吗

```
Allocated <N> bytes of fabric host memory, base=0x..., alignment=16777216
```

或 VRAM：

```
VRAM Segment allocated <N> bytes as an exportable fabric allocation, base=..., alignment=...
```

失败时不会静默回落，会看到：

```
Failed to allocate <N> bytes of fabric host memory for protocol nvlink;
unset MC_STORE_HOST_FABRIC to use plain host memory instead.
```

这是**设计如此** —— 回落到 `aligned_alloc` 只会把失败推迟到远端读 miss,见 `design.md` §7。

### 7.3 vLLM 侧

```
[kv-fabric-patch] compiled /tmp/kv_fabric_alloc.so
[kv-fabric-patch] attn_utils.py patched
KV cache: using fabric-VMM MemPool (MNNVL exportable)
```

## 8. 常见问题

**strict 模式下所有写都失败**

先看有没有这条：

```
strict_rack with rack_id=<x> but the master knows of no rack-bearing segment at all
```

有则说明没有任何 segment 报告过 rack。两个原因：store 节点没设 `rack_id`;或 master 恢复了 pre-rack 快照而 store 节点没重新 mount。

**`strict_rack=true` 但看到"falling back to non-strict"**

`rack_id` 空。strict 模式要求 `rack_id` 非空，否则退回 soft 并告警。

**同机架却走了 RDMA**

按顺序查：两侧 `MOONCAKE_RACK_ID` 是否**完全相同**(大小写敏感);segment 的 protocol 是否为 `nvlink`;是否设了 `MC_DISABLE_NVLINK`;nvlink transport 是否真的装上了(`usesFabricMem()` 为 false 时会退化成同主机门控)。

**fabric 分配失败：`cuMemCreate failed`**

依次查：`nvidia-smi -q | grep -i fabric` 看设备是否支持;`NVIDIA_IMEX_CHANNELS=0` 是否注入;驱动版本。host fabric 还要看 `cudaDevAttrHostNumaId` —— 驱动不暴露该属性会报"reports no host NUMA node"并返回 `nullptr`(不猜 node 0)。

**`MC_STORE_HOST_FABRIC=1` 完全没反应**

三个可能：构建没开 `USE_CUDA`;构建开了 `USE_VRAM_SEGMENT`(host fabric 不编入，见 §1);segment 的 protocol 不是 `nvlink`。

**`MC_STORE_USE_HUGEPAGE` 干扰**

它是**存在即生效**(设 `0` 也算开),会把分配从 fabric 路径截走。要用 fabric 就 `unset` 它，而不是设成 `0`。

## 9. 已验证 / 未验证

| 项 | 状态 |
| --- | --- |
| `USE_CUDA=ON` 全量编译 | 已验证(115 机器) |
| `host_fabric_config_test` 6 例 | 通过 |
| `vram_fabric_config_test` 6 例 | 通过(未被破坏) |
| `buffer_allocator_test` 21 例 | 通过(未被破坏) |
| `cuMemCreate(HOST_NUMA + FABRIC)` 真实行为 | **未验证** —— 只过了编译器 |
| 多节点 NVLink domain 端到端 | **未验证** —— 需要相应硬件 |
| `USE_VRAM_SEGMENT=ON` 构建 | **本轮未编** |

最后三项需要真实 NVLink 拓扑或额外构建配置，本轮没有条件做。
