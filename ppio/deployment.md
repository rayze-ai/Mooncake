# 编译与部署

对应 `design.md` / `todo.md`。分三部分:阶段 1-2 只改配置;阶段 3 编 Mooncake;阶段 4-5 部署与验证。

## 一、阶段 1-2:只改配置(不依赖 Mooncake)

### P 和 D 两份 YAML 各改两处

**env 加:**

```yaml
- name: UCX_CUDA_IPC_ENABLE_MNNVL
  value: 'y'
- name: UCX_LOG_LEVEL          # 验证期,通了删掉
  value: info
```

**CLI 加:**

```
--enable-cumem-allocator
```

其他一行不动。`UCX_TLS` 里 `cuda_ipc` 已在;`NCCL_CUMEM_ENABLE=1` 是 NCCL 自己的
buffer,与 KV cache 无关,保留即可。

### 重启后按顺序看四样

**① env 名被认了吗**

```bash
grep -i 'unused environment' <log>
```

有输出 → `UCX_CUDA_IPC_ENABLE_MNNVL` 名字错(UCX 会点名)。停,回来讨论。

**② clique 认出来了吗**

```bash
grep -i 'fabric_info' <log>
```

预期 `fabric_info: state=3 status=0 uuid=2e991d96-...`,UUID 与 `nvidia-smi -q` 的
ClusterUUID 一致。info 级别没有就换 `UCX_LOG_LEVEL=debug` 再起。

**③ KV cache 是 fabric 内存吗**

```bash
grep -i 'does not have fabric property' <log>
```

| 结果 | 含义 |
| --- | --- |
| 无 | cumem 拿到了 FABRIC handle,UCX 能导出 |
| 有 | cumem 静默回落到了 POSIX FD(`cumem_allocator.cpp:157-160`)。查 IMEX:`ls /dev/nvidia-caps-imex-channels/`,宿主机 `systemctl status nvidia-imex` |

**④ 计数器坐实**

D 节点上,一次 PD 交接前后各一次:

```bash
for d in /sys/class/infiniband/mlx5_bond_*/ports/1; do
  echo "$d: $(cat $d/hw_counters/rx_write_requests) $(cat $d/hw_counters/rx_read_requests)"
done
nvidia-smi nvlink -gt d -i 0 | grep -i rx
```

RDMA 不涨、NVLink Rx 涨 → 阶段 1 通。用 `hw_counters/rx_*_requests` 而非
`port_rcv_data`:前者只对 RDMA verb 计数,不被 TCP 污染。EP 流量本来就在 NVLink 上,
所以看的是 **RDMA 那边不涨**。

### 硬件前提(已核准,host-10-0-3-71)

```
nvidia-smi -q | grep -A4 Fabric   → State: Completed, CliqueId 32766, 四卡 ClusterUUID 一致
cuMemCreate(DEVICE, FABRIC)       → rc=0
cuMemExportToShareableHandle      → rc=0
```

pod 是 `privileged: true`,`/dev` 全量可见,**不需要** `NVIDIA_IMEX_CHANNELS=0`。

## 二、阶段 3:编 Mooncake

### 依赖

Ubuntu 22.04 最小集(每项都是撞过的):

```bash
apt-get install -y \
  libyaml-cpp-dev libgoogle-glog-dev libgflags-dev \
  libzstd-dev libxxhash-dev libjsoncpp-dev \
  libcurl4-openssl-dev libssl-dev \
  libibverbs-dev librdmacm-dev libnuma-dev \
  libboost-all-dev libmsgpack-dev \
  pybind11-dev python3-pybind11 liburing-dev
```

能跑 `dependencies.sh` 优先用它。

### 配置

```bash
mkdir -p build && cd build
cmake .. -DUSE_CUDA=ON -DUSE_MNNVL=ON -DBUILD_UNIT_TESTS=ON
```

两个开关都不能省:

| 开关 | 省了会怎样 |
| --- | --- |
| `USE_CUDA=ON` | host fabric 分配器只编到非 CUDA 桩分支 |
| **`USE_MNNVL=ON`** | **默认 OFF**。nvlink transport 不编入 → `nvlinkUsesFabricMem()` 恒 false → `MC_STORE_HOST_FABRIC` 没有生效路径。日志上 env 解析正常,分配走的还是 `aligned_alloc` |
| `SKBUILD=ON` | **不要加**(115 上无系统 pybind11,加了 `find_package` 直接失败)。它只切 pybind11 来源,子模块在就不需要 |

`USE_MNNVL` 会隐式 `set(USE_CUDA ON)`,但显式写上更清楚。

**不要**开 `USE_VRAM_SEGMENT`:host fabric 的编译条件是
`defined(USE_CUDA) && !defined(USE_VRAM_SEGMENT)`,开了就编不进去。store 是 DRAM 池,
用不到 VRAM segment。

### 编译

```bash
make -j$(nproc)

# 只验证 fabric 相关(会连带编出依赖,足以验证编译正确性):
make host_fabric_config_test nvlink_fabric_ready_test buffer_allocator_test -j$(nproc)
```

本地(4090 机器)缺 folly(`Slab.h`),编不了 `mooncake_store`。在 CUDA 机器上编。

### 测试

```bash
cd build/mooncake-store/tests
./host_fabric_config_test
./nvlink_fabric_ready_test
./buffer_allocator_test

# 或
cd build && ctest -R "fabric|buffer_allocator" --output-on-failure
```

`host_fabric_config_test` 里 `InvalidValueFallsBackDisabled` 会打一条 WARNING,是预期的。

### pre-commit

```bash
pre-commit run --files <改动的文件列表>
```

**不用 `--all-files`**。cmake-format 有重排 `mooncake-store/tests/CMakeLists.txt` 里
无关行的前科,重排了就手工还原,只留自己加的那行。

### 出 wheel

```bash
cd <repo>
OUTPUT_DIR=dist ./scripts/build_wheel.sh
```

**必须在 arm64 上构建** —— GB300 是 Grace。产物名形如
`mooncake_transfer_engine_cuda13-<ver>-cp312-cp312-manylinux_2_39_aarch64.whl`。

推到内部 server,更新 `MOONCAKE_VERSION`:

```
http://vllm-code-server.ruizi-k3pd:9099/mooncake.whl
```

## 三、阶段 4:部署

### store 进程(`mc_store_rest_server`)

这是唯一 mount segment 的进程,fabric 相关 env **全设在它身上**。

```yaml
env:
- name: MC_STORE_HOST_FABRIC
  value: '1'
- name: MC_FORCE_MNNVL
  value: '1'
# 非 privileged 时才需要:
# - name: NVIDIA_IMEX_CHANNELS
#   value: '0'
```

json:

```json
{
  "protocol": "rdma",
  "global_segment_size": "<按容量>",
  "local_buffer_size": "1gb",
  ...
}
```

`protocol` 写 `rdma`,**不是** `nvlink` —— Store 不认 `nvlink` 这个值(`IsHostStoreSegmentProtocol()`
白名单没它,写了丢 `cudaHostRegister`)。fabric 由 TE 实际装的 transport 决定,与 protocol
字符串无关。

确认 `MC_STORE_USE_HUGEPAGE` **未设**:它存在即生效(设 `0` 也算开),会把分配截走。

### Prefill

在阶段 1-2 的基础上再加:

```yaml
- name: MC_FORCE_MNNVL      # 让 P 的 TE 装 nvlink 而非 rdma
  value: '1'
```

connector 不动(已是 `MultiConnector` + Nixl producer + Store kv_both)。换新 wheel。

### Decode

在阶段 1-2 的基础上:

```yaml
- name: MC_FORCE_MNNVL
  value: '1'
- name: MOONCAKE_CONFIG_PATH
  value: /path/to/mooncake.json      # 同 P:standalone-store, global_segment_size=0
```

CLI:

```
--enable-prefix-caching             # 原来是 --no-enable-prefix-caching,必须改
--kv-transfer-config '{
  "kv_connector":"MultiConnector","kv_role":"kv_consumer",
  "kv_connector_extra_config":{"connectors":[
    {"kv_connector":"NixlConnector","kv_role":"kv_consumer",
     "kv_load_failure_policy":"fail",
     "kv_connector_extra_config":{"kv_lease_duration":300,"engine_ttl":0}},
    {"kv_connector":"MooncakeStoreConnector","kv_role":"kv_consumer",
     "kv_connector_extra_config":{"enable_lookup":true}}
  ]}}'
```

`--enable-prefix-caching` 不开,`MooncakeStoreConnector` 的 lookup 没有前缀块可匹配,
接了也白接。

## 四、阶段 5:验证 store 路径

**store 进程日志,两条都要有:**

```
Using NVLink transport (forced or no HCA detected)        ← TE 装上了
Allocated <N> bytes of fabric host memory, base=0x...     ← segment 带 handle 了
```

只有第一条 → `MC_STORE_HOST_FABRIC` 没生效,按下面排查。

**P、D 进程日志:**

```
Using NVLink transport (forced or no HCA detected)
```

出现 `Using RDMA transport` 就是 `MC_FORCE_MNNVL` 没设到这个进程上。

**功能:**打两轮同前缀请求,第二轮 D 侧日志有 store 命中、无 miss。

**计数器:**命中期间 RDMA 不涨、NVLink Rx 涨。

**对照实验:**store 关 `MC_STORE_HOST_FABRIC` 重启,同样请求应看到 RDMA 涨回来。
这一步证明差异确实来自 fabric,不是别的。

## 五、排查

**`MC_STORE_HOST_FABRIC=1` 完全没反应**,按顺序:

1. 构建没开 `USE_CUDA`
2. **构建没开 `USE_MNNVL`**(最容易漏,默认 OFF)
3. 构建开了 `USE_VRAM_SEGMENT`
4. 该进程日志没有 `Using NVLink transport` → `MC_FORCE_MNNVL` 没设到它
5. 设在了 vLLM 进程上 —— `standalone-store` 下 vLLM 不 mount,该设在 store 进程
6. `MC_STORE_USE_HUGEPAGE` 存在

**`cuMemCreate failed`**:`nvidia-smi -q | grep -A4 Fabric` 看 State;宿主机
`systemctl status nvidia-imex`;非 privileged 容器查 `NVIDIA_IMEX_CHANNELS`。host fabric
还会查 `cudaDevAttrHostNumaId`,驱动不暴露会报 "reports no host NUMA node" 并返回
`nullptr`(不猜 node 0)。

**UCX 日志有 `does not have fabric property`**:cumem 回落了。同上查 IMEX。

**注册后远端读 miss、日志无错**:阶段 3 之后这条路默认已是硬错 —— segment 内存不是
VMM 分配时 `MountSegment` 直接失败并打 ERROR。还能看到"mount 成功但远端 miss"只有
一种可能:store 进程设了 `MC_NVLINK_TOLERATE_NON_FABRIC`,日志里会有对应 WARNING。
`unset` 它让失败提前到启动。
