# syntax=docker/dockerfile:1.7
#
# Rack-aware Mooncake image for GB300 NVL72 (4 racks x 18 nodes).
#
# Derived from docker/mooncake.Dockerfile, which is left untouched. The ONLY
# build difference is the two extra cmake switches that enable the
# fabric-handle VRAM path:
#     -DUSE_VRAM_SEGMENT=ON -DUSE_MNNVL=ON
# Rack-aware placement itself needs no build flag -- it is pure placement-layer
# logic and is always compiled in. Build this image only if you also want to
# evaluate NVLink in the data path; otherwise docker/mooncake.Dockerfile is
# enough and carries less risk.
#
# GB300 is Grace (aarch64): build this on an arm64 host (or with
# `docker buildx build --platform linux/arm64`). An x86_64 image will not run.
#
# Build from the REPOSITORY ROOT, not from ppio/ -- the build context must be the
# repo root because the builder stage does `COPY . /workspace`:
#     docker build -f ppio/mooncake-rack.Dockerfile -t mooncake:rack .
#
# See ppio/build.md for build instructions, ppio/config.md for the
# environment-variable tables, ppio/index.html for the overview, and
# 20260918.md section 8.4 for why MC_FORCE_MNNVL must NOT be set on a node
# that also needs cross-rack reads.
###############################################################################

ARG CUDA_VERSION=12.8.1
ARG UBUNTU_VERSION=22.04

FROM nvidia/cuda:${CUDA_VERSION}-devel-ubuntu${UBUNTU_VERSION} AS builder

ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1

ARG PYTHON_VERSION=3.10
ARG PYPA_INDEX_URL=https://bootstrap.pypa.io
ARG CMAKE_BUILD_TYPE=Release
ARG CLEAN_BUILD_ARTIFACTS=0
# Run the rack-affinity and fabric-switch unit tests during the build. They
# have never been executed in a full-dependency environment, so this is the
# first place they actually run. Pass --build-arg RUN_TESTS=0 to skip if a
# test fails for reasons unrelated to the change (the builder has no GPU).
ARG RUN_TESTS=1

ENV PYTHON_VERSION=${PYTHON_VERSION} \
    RUN_TESTS=${RUN_TESTS} \
    PATH="/usr/local/go/bin:${PATH}"

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        git \
        ninja-build \
        software-properties-common \
        pkg-config && \
    add-apt-repository -y ppa:deadsnakes/ppa && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        python${PYTHON_VERSION} \
        python${PYTHON_VERSION}-dev \
        python${PYTHON_VERSION}-venv && \
    curl -sS ${PYPA_INDEX_URL}/get-pip.py | python${PYTHON_VERSION} && \
    update-alternatives --install /usr/bin/python  python  /usr/bin/python${PYTHON_VERSION} 1 && \
    update-alternatives --install /usr/bin/python3 python3 /usr/bin/python${PYTHON_VERSION} 1 && \
    apt-get purge -y --auto-remove software-properties-common && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . /workspace

RUN bash dependencies.sh -y

# WITH_EP is deliberately left off: the EP/PG device extensions are unrelated to
# rack affinity and pull in a torch build that roughly doubles image build time.
# Add -DWITH_EP=ON back if you use the EP path.
#
# USE_VRAM_SEGMENT and USE_MNNVL each force USE_CUDA=ON (common.cmake:223-238).
# Both are required for MC_STORE_VRAM_FABRIC to have any effect; with only one of
# them the switch degrades to cudaMalloc -- it does log a warning, but the
# segment is then silently unreachable over NVLink, which is the exact failure
# this feature exists to remove. The greps below turn that misconfiguration into
# a build failure instead of a runtime surprise.
#
# Kept in a single layer, like docker/mooncake.Dockerfile: build/ is removed only
# after auditwheel has resolved libraries out of it, so the large intermediate
# tree never lands in the builder image or the BuildKit cache.
RUN mkdir -p build && \
    cd build && \
    cmake -G Ninja .. \
        -DBUILD_UNIT_TESTS=ON \
        -DUSE_HTTP=ON \
        -DUSE_ETCD=ON \
        -DUSE_CUDA=ON \
        -DUSE_VRAM_SEGMENT=ON \
        -DUSE_MNNVL=ON \
        -DSTORE_USE_ETCD=ON \
        -DPython3_EXECUTABLE=/usr/bin/python${PYTHON_VERSION} \
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} 2>&1 | tee /tmp/cmake.log && \
    grep -q 'VRAM SEGMENT is ON' /tmp/cmake.log && \
    grep -q 'Multi-Node NVLink support is enabled' /tmp/cmake.log && \
    grep -q 'CUDA support is enabled' /tmp/cmake.log && \
    export LIBRARY_PATH=/usr/local/cuda/lib64/stubs:$LIBRARY_PATH && \
    cmake --build . && \
    if [ "${RUN_TESTS}" = "1" ]; then \
        ctest -R 'vram_fabric_config_test|replica_selection_test|segment_test' \
              --output-on-failure; \
    fi && \
    cd /workspace && \
    export PATH=/usr/local/nvidia/bin:/usr/local/nvidia/lib64:$PATH && \
    export LD_LIBRARY_PATH=/usr/local/cuda/lib64/stubs:$LD_LIBRARY_PATH && \
    mkdir -p build/mooncake-transfer-engine/nvlink-allocator && \
    cd mooncake-transfer-engine/nvlink-allocator && \
    bash build.sh ../../build/mooncake-transfer-engine/nvlink-allocator/ && \
    cd /workspace && \
    OUTPUT_DIR=dist ./scripts/build_wheel.sh && \
    if [ "${CLEAN_BUILD_ARTIFACTS}" = "1" ]; then rm -rf build; fi

###############################################################################
# Stage 2: runtime
###############################################################################
FROM nvidia/cuda:${CUDA_VERSION}-runtime-ubuntu${UBUNTU_VERSION} AS runtime

ENV DEBIAN_FRONTEND=noninteractive \
    PYTHONUNBUFFERED=1 \
    PIP_NO_CACHE_DIR=1

ARG PYTHON_VERSION=3.10
ARG PYPA_INDEX_URL=https://bootstrap.pypa.io
ENV PYTHON_VERSION=${PYTHON_VERSION}

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        software-properties-common \
        ibverbs-providers \
        rdma-core \
        libibverbs1 \
        librdmacm1 \
        libnuma1 \
        liburing2 \
        libyaml-0-2 \
        libcurl4 && \
    add-apt-repository -y ppa:deadsnakes/ppa && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        python${PYTHON_VERSION} && \
    curl -sS ${PYPA_INDEX_URL}/get-pip.py | python${PYTHON_VERSION} && \
    update-alternatives --install /usr/bin/python  python  /usr/bin/python${PYTHON_VERSION} 1 && \
    update-alternatives --install /usr/bin/python3 python3 /usr/bin/python${PYTHON_VERSION} 1 && \
    apt-get purge -y --auto-remove software-properties-common curl && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /workspace/mooncake-wheel/dist /tmp/mooncake-wheel
RUN python${PYTHON_VERSION} -m pip install --no-cache-dir /tmp/mooncake-wheel/*.whl && \
    rm -rf /tmp/mooncake-wheel /root/.cache/pip

# Deliberately NOT baked in, because each of these is either per-node or
# dangerous as a default:
#   MOONCAKE_RACK_ID      -- per node, must match the NVLink/IMEX domain
#   MOONCAKE_STRICT_RACK  -- per role (write side only)
#   MC_STORE_VRAM_FABRIC  -- opt-in, single-rack validation only
#   MC_FORCE_MNNVL        -- installs nvlink INSTEAD OF rdma; no cross-rack path
# Pass them with `docker run -e ...` per node. See 20260918-deploy.md.

CMD ["/bin/bash"]
