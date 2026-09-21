# syntax=docker/dockerfile:1.7
#
# Build the Mooncake wheel and export ONLY the .whl files to the host.
#
# Unlike ppio/mooncake-rack.Dockerfile (which produces a runnable image), the
# final stage here is `FROM scratch` holding nothing but mooncake-wheel/dist, so
# `--output type=local,dest=<dir>` drops the wheel straight onto disk instead of
# burying it in an image layer.
#
# Both scenarios come from this one file, via GPU_FLAGS:
#
#   scenario A (rack affinity only -- no GPU switches needed):
#     docker buildx build --builder mc-arm --platform linux/arm64 \
#       -f ppio/wheel-only.Dockerfile \
#       --output type=local,dest=ppio/dist-base .
#
#   scenario B (adds fabric handle):
#     docker buildx build --builder mc-arm --platform linux/arm64 \
#       -f ppio/wheel-only.Dockerfile \
#       --build-arg GPU_FLAGS="-DUSE_VRAM_SEGMENT=ON -DUSE_MNNVL=ON" \
#       --output type=local,dest=ppio/dist-rack .
#
# Build from the REPOSITORY ROOT: the builder stage does `COPY . /workspace`.
# GB300 is Grace (aarch64), so pass --platform linux/arm64. On an x86_64 host
# that needs qemu binfmt registered first:
#     docker run --privileged --rm tonistiigi/binfmt --install arm64
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
# Empty = scenario A. Pass "-DUSE_VRAM_SEGMENT=ON -DUSE_MNNVL=ON" for scenario B.
ARG GPU_FLAGS=""
# Run the rack-affinity and fabric-switch unit tests during the build. They have
# never been executed in a full-dependency environment, so this is the first
# place they actually run. Pass RUN_TESTS=0 to skip (the builder has no GPU).
ARG RUN_TESTS=1

ENV PYTHON_VERSION=${PYTHON_VERSION} \
    GPU_FLAGS=${GPU_FLAGS} \
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

# WITH_EP is left off deliberately: the EP/PG device extensions are unrelated to
# rack affinity and pull in a torch build that roughly doubles build time. Add
# -DWITH_EP=ON to GPU_FLAGS if you need the EP path.
#
# When GPU_FLAGS requests the fabric path, both switches must have taken effect;
# with only one of them MC_STORE_VRAM_FABRIC degrades to cudaMalloc and the
# segment is silently unreachable over NVLink. The grep turns that into a build
# failure rather than a runtime surprise. Scenario A skips the check.
RUN mkdir -p build && \
    cd build && \
    cmake -G Ninja .. \
        -DBUILD_UNIT_TESTS=ON \
        -DUSE_HTTP=ON \
        -DUSE_ETCD=ON \
        -DSTORE_USE_ETCD=ON \
        ${GPU_FLAGS} \
        -DPython3_EXECUTABLE=/usr/bin/python${PYTHON_VERSION} \
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} 2>&1 | tee /tmp/cmake.log && \
    if echo "${GPU_FLAGS}" | grep -q USE_MNNVL; then \
        grep -q 'VRAM SEGMENT is ON' /tmp/cmake.log && \
        grep -q 'Multi-Node NVLink support is enabled' /tmp/cmake.log && \
        grep -q 'CUDA support is enabled' /tmp/cmake.log; \
    fi && \
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
    OUTPUT_DIR=dist ./scripts/build_wheel.sh

###############################################################################
# Export stage: nothing but the wheels, so --output type=local yields just those
###############################################################################
FROM scratch AS wheel
COPY --from=builder /workspace/mooncake-wheel/dist/ /
