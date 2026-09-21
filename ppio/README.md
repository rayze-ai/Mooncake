# ppio

Mooncake 在 4 × GB300 NVL72 上的机架感知部署方案。

**先看这个**

| 文件 | 内容 |
| --- | --- |
| `index.html` | **总览**：架构图、PD 读写路径图、每个组件怎么构建与配置 |

**方案设计**

| 文件 | 内容 |
| --- | --- |
| `20260918.md` | 方案设计与可行性分析：为什么这么做、哪里还不通、fabric handle 实现细节 |
| `20260918.html` | 方案论证的图示版（拓扑、数据路径、阻塞点） |
| `20260918-deploy.md` | 编译 / 部署拓扑 / 配置 / 上线验证 / 常见问题 |

**构建与配置**

| 文件 | 内容 |
| --- | --- |
| `build.md` | 三个组件（master / store server / client）怎么构建 |
| `config.md` | NVL72 每个组件的完整可抄配置 |
| `mooncake-rack.Dockerfile` | 带 `USE_VRAM_SEGMENT` + `USE_MNNVL` 的镜像定义 |

先在浏览器打开 `index.html` 看一遍全局，再按下面的最短路径动手。

## 最短路径

**只要机架感知**（推荐先做，收益确定）：

```bash
# 镜像：用仓库现成的，一行都不用改
docker build -f docker/mooncake.Dockerfile -t mooncake:base .

# 或者 wheel
sudo bash dependencies.sh -y
mkdir -p build && cd build && cmake .. -G Ninja && cmake --build . && cd ..
OUTPUT_DIR=dist ./scripts/build_wheel.sh
```

机架感知**不需要任何编译开关**，纯放置层逻辑。配好 `MOONCAKE_RACK_ID` 即可。

**还要验证 fabric handle**（谨慎，见下）：

```bash
# 注意：从仓库根目录执行，不要 cd 进 ppio/
docker build -f ppio/mooncake-rack.Dockerfile -t mooncake:rack-arm64 .
```

## 三个必须知道的坑

1. **GB300 是 Grace，aarch64。** 镜像必须在 arm64 上构建，x86_64 跑不起来。
2. **`MC_FORCE_MNNVL` 设了就没有 RDMA。** transport 安装是互斥 if/else，
   该节点跨机架读会直接失败且无回落。生产节点一律不设。
   详见 `config.md` 第 5 节。
3. **`MC_STORE_USE_HUGEPAGE` 存在即生效**（设 `0` 也算开），会把分配从 VRAM
   路径截走，让 `MC_STORE_VRAM_FABRIC` 等于没开。

## 当前能力边界

- 机架感知放置：**已实现可用**，可以直接上线
- fabric handle 分配侧：**已实现，带开关**，但只能在不需要跨机架通信的
  隔离节点上验证
- NVLink 进跨机架数据路径：**没有可用配置**，选路层未实现（`20260918.md` §8.4）
