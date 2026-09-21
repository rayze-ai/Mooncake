# 构建：三个组件

Mooncake 部署有三个角色，**构建需求完全不同**。先认清这点，能省掉一半工作：

| 组件 | 产物 | 需要 GPU 开关？ | 说明 |
| --- | --- | --- | --- |
| **master** | `mooncake_master` 二进制 | **不需要** | 只管元数据与放置决策，不碰任何数据；链接 ibverbs 但**不链接 CUDA** |
| **store server** | wheel（`store.so` + `engine.so` + Python 包） | 仅在要 fabric 时需要 | **它就是一个贡献内存的 client**，没有独立二进制 |
| **client**（prefill / decode） | 同一个 wheel | 否 | 只分配 `local_buffer`，不贡献 `global_segment` |

关键事实：**store server 与 client 是同一份 wheel**，区别只在配置
（`global_segment_size` 给不给、`strict_rack` 开不开）。所以真正要构建的只有两样：
一个 master 二进制 + 一个 wheel。而 `mooncake_master` 也在那个 wheel 里
（`scripts/build_wheel.sh:63`），所以**一次构建就够**。

## 1. master

### 需要什么

不需要 `USE_VRAM_SEGMENT`、不需要 `USE_MNNVL`、不需要 CUDA。
master 只维护 `RackSegmentIndex`（segment name → rack_id）并做放置决策，
数据面完全不经过它。

target 定义在 `mooncake-store/src/CMakeLists.txt:642`，链的是
`mooncake_store_master`、cachelib、ibverbs、glog、gflags、JsonCpp、yalantinglibs。
`MASTER_EXTRA_LIBS` 只在 `STORE_USE_JEMALLOC` 时非空。

### 构建

```bash
# 容器：仓库现成的，够用
docker build -f docker/master.Dockerfile \
  --build-arg MOONCAKE_VERSION=<version> -t mooncake-master .
```

`docker/master.Dockerfile` 从 PyPI 拉发布版、剥掉 EP/PG 扩展、以 uid 65532 非 root 运行。
要自己编的话，master 二进制在主 wheel 里，走下面第 2 节即可。

源码构建单独的 master：

```bash
mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build . --target mooncake_master
# 产物：build/mooncake-store/src/mooncake_master
```

HA 部署需要 `-DSTORE_USE_ETCD=ON` 或 `-DSTORE_USE_REDIS=ON`。

## 2. store server 与 client（同一个 wheel）

### 两个场景

| 场景 | cmake 开关 | 适用 |
| --- | --- | --- |
| **A. 只要机架感知** | 无需任何 GPU 开关 | **推荐先做**。机架感知是纯放置层逻辑，始终编入 |
| **B. 加 fabric handle** | `-DUSE_VRAM_SEGMENT=ON -DUSE_MNNVL=ON` | 只在要验证 NVLink 分配侧时 |

能力上，**场景 B 是 A 的超集**：机架感知始终编入，所以带 fabric 开关的 wheel
同时具备两个能力，要两个能力只需要一个 wheel。

但开关不是免费的。⚠️ **`USE_VRAM_SEGMENT=ON` 会让 host DRAM 分配路径变成死代码。**
`client_buffer_allocation.cpp:191-197` 里那个 `#ifdef USE_VRAM_SEGMENT` 分支是
**无条件 return**，下面的 `aligned_alloc`（host DRAM）永远走不到，
而且**没有运行时开关可以关掉它**：

```
#ifdef USE_VRAM_SEGMENT
    auto ret = allocate_vram_memory(total_size, protocol, alignment);
    ...
    return *ret;          // ← 无条件返回，不看 protocol
#endif
    return aligned_alloc(alignment, total_size);   // ← 死代码
```

后果：这个 wheel 上**每一个** store server 的 `global_segment` 都从 GPU 显存分配。
`protocol=rdma`（NVL72 生产配置）时的实际路径是：不是 `nvlink_intra` → 跳过；
`use_fabric_vram("rdma")` 返回 false → 跳过 fabric 分支 → 落到
`cudaMalloc(total_size)`。也就是说配 `global_segment_size: 64gb` 就是向单张 GPU
`cudaMalloc` 64 GB 显存，这块显存从推理负载手里拿走。

所以选哪个场景不是「要不要多一个能力」，而是**store server 贡献 host DRAM 还是
GPU 显存**这个部署决策：

| 你想让 store server 贡献 | 用哪个场景 |
| --- | --- |
| host DRAM（常规容量模型） | **A**。fabric handle 拿不到，但机架感知完整可用 |
| GPU 显存 | B。fabric handle 的前提就是 VRAM segment（`20260918.md` 路线 A） |

fabric handle 没有「host DRAM + fabric」的组合——那是路线 B（EGM），**未实现**。
所以想要 fabric，就必须接受 store server 用显存。

client 侧（prefill/decode）不受影响：它们只分配 `local_buffer`，不走这条路径。

### 依赖

```bash
sudo bash dependencies.sh -y
```

联网：装系统包、初始化 submodule、安装 Go。gtest 不在其中，
由 CMake FetchContent 在 configure 阶段拉（`mooncake-common/FindGTest.cmake`，GoogleTest 1.17.0）。

场景 B 还需 CUDA 12.1+：

```bash
export LIBRARY_PATH=/usr/local/cuda/lib64:$LIBRARY_PATH
export LD_LIBRARY_PATH=/usr/local/cuda/lib64:$LD_LIBRARY_PATH
```

### 2a. 容器

```bash
# 场景 A：仓库现成的，一行都不用改
docker build -f docker/mooncake.Dockerfile -t mooncake:base .

# 场景 B：本目录的（注意从仓库根执行，构建上下文必须是仓库根）
docker build -f ppio/mooncake-rack.Dockerfile -t mooncake:rack-arm64 .
```

**GB300 是 Grace，aarch64。** 镜像必须在 arm64 上构建，x86_64 跑不起来。
跨平台构建需要 qemu，很慢，只适合应急：

```bash
docker buildx build --platform linux/arm64 \
  -f ppio/mooncake-rack.Dockerfile -t mooncake:rack-arm64 --load .
```

`ppio/mooncake-rack.Dockerfile` 与 `docker/mooncake.Dockerfile` 的差异只有四处：

1. 加 `-DUSE_VRAM_SEGMENT=ON -DUSE_MNNVL=ON`
2. cmake 后 grep 三行 STATUS，开关没生效**直接构建失败**
3. `BUILD_UNIT_TESTS=ON` 并在 builder 阶段跑 `vram_fabric_config_test` /
   `replica_selection_test` / `segment_test`——这些用例此前从未在有依赖的环境执行过
4. 去掉 `WITH_EP=ON`（EP/PG 扩展与机架无关，会拉 torch，构建时间约翻倍）

build arg：

| arg | 默认 | 说明 |
| --- | --- | --- |
| `CUDA_VERSION` | `12.8.1` | fabric 需要 12.1+。12.8 含 Blackwell Ultra（sm_103） |
| `PYTHON_VERSION` | `3.10` | 要与推理框架的 Python 版本一致 |
| `RUN_TESTS` | `1` | builder 阶段跑单测。构建机无 GPU，个别用例失败时传 `0` |
| `CLEAN_BUILD_ARTIFACTS` | `0` | `1` 则 auditwheel 后删 `build/` |

构建期用 `/usr/local/cuda/lib64/stubs` 的 stub libcuda 链接（构建机没驱动），
运行时由 nvidia-container-runtime 注入真实 libcuda。这是原 Dockerfile 的做法。

### 2b. wheel

```bash
mkdir -p build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUSE_VRAM_SEGMENT=ON -DUSE_MNNVL=ON      # 场景 B 才加这两行
cmake --build .
cd ..

# 可选：仅推理框架侧的 torch 可插拔分配器需要，与 store 的 fabric segment 无关
mkdir -p build/mooncake-transfer-engine/nvlink-allocator
cd mooncake-transfer-engine/nvlink-allocator
bash build.sh ../../build/mooncake-transfer-engine/nvlink-allocator/
cd ../..

OUTPUT_DIR=dist ./scripts/build_wheel.sh
```

产物在 `mooncake-wheel/dist/`，包含 `engine.so`、`store.so`、
**`mooncake_master` 二进制**、`mooncake_client`、Python 包。

关于 `nvlink_allocator.so`：`scripts/build_wheel.sh:108-121` 只在文件存在时才拷，
缺失时打印 "Skipping nvlink_allocator.so (not built - likely ARM64 or non-CUDA build)"。
那句 "likely ARM64" 只是文案推测，`nvlink-allocator/build.sh` 里**没有架构门控**，
aarch64 上照样能编。更重要的是它**与 store 的 fabric segment 无关**——
只被 `mooncake-integration/allocator.py` 用作推理框架侧的 torch 可插拔分配器。
store 的 fabric 分配走 `NvlinkTransport::allocatePinnedLocalMemory`，编在 `engine.so` 里。

### 验证场景 B 的开关真的生效

cmake 输出必须有这三行：

```
-- VRAM SEGMENT is ON
-- Multi-Node NVLink support is enabled
-- CUDA support is enabled
```

两个开关各自都会强制 `USE_CUDA=ON`（`mooncake-common/common.cmake:223-238`）。

**互斥约束**：`USE_MNNVL` 不能与 `USE_HIP` / `USE_MUSA` / `USE_UBSHMEM` 同时开。
那些组合下 `gpu_vendor/mnnvl.h` 分发到别的厂商后端，而那些后端没有带对齐的
fabric 分配接口，开关会降级为 `cudaMalloc` 并打 WARNING。

### 容易搞混的两个 NVLink 开关

| 开关 | 协议名 | 范围 | 机制 |
| --- | --- | --- | --- |
| `USE_MNNVL` | `nvlink` | **跨机**（同一 NVLink 域 / 同机架 18 台） | fabric handle（`cuMemCreate` + `CU_MEM_HANDLE_TYPE_FABRIC`） |
| `USE_INTRA_NVLINK` | `nvlink_intra` | **仅单机内** | CUDA IPC handle（`cudaIpcGetMemHandle`） |

本方案要 `USE_MNNVL`。`nvlink_intra` 虽有个 `allocateFabricMemory_intra` 宏，
实现是纯 `cudaMalloc`，日志明说 "memory will NOT be exportable"，**跨机用不了**。

## 3. 跑单测

这些用例此前从未在有依赖的环境执行过，**首次部署务必跑一遍**：

```bash
cd build
ctest -R vram_fabric_config_test --output-on-failure
ctest -R 'replica_selection_test|segment_test' --output-on-failure
cd .. && cd python && python3 -m unittest tests.unit.test_mooncake_config
```

## 4. 安装 wheel

```bash
pip install mooncake-wheel/dist/*.whl
sudo apt-get install -y ibverbs-providers rdma-core libibverbs1 librdmacm1 \
                        libnuma1 liburing2 libyaml-0-2 libcurl4
```

`ibverbs-providers` 必装且版本要与 `libibverbs1` 匹配（同一 rdma-core 源码包，
共享私有 provider ABI）。缺了它 `import mooncake.engine` 直接失败——
`engine.so` 带着对 `libmlx5.so.1` 的硬 `DT_NEEDED`。wheel 刻意不 vendor RDMA 库。

配置见 `config.md`，总览见 `index.html`。
