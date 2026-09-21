## Description

Makes Mooncake aware of NVLink domain (rack) boundaries in placement, replica
selection, and transport routing, and adds fabric-handle allocation so the
memory those paths select is actually reachable over cross-node NVLink (MNNVL).

Two independent halves; the first is useful on its own:

**1. Rack awareness** (no build flags, always compiled in)

Within a GB300 NVL72 rack, GPUs are fully connected by NVLink; across racks
only RDMA exists. Placement and routing previously could not see that
difference, so a read that could have gone over NVLink often went over RDMA.

- `multi_transport_locality.h` (new): header-only predicates for the routing
  decision, including a tri-state `RackReachability {Reachable, Unreachable,
  Unknown}`. `Unknown` (either side's rack id unset) is deliberately distinct
  from `Unreachable` (both set and different): callers downgrade on both, but
  only `Unreachable` is a statement about the topology and warrants an error
  when no fallback transport exists. Same-host targets are always `Reachable`
  regardless of rack ids, so single-node deployments are untouched.
- `multi_transport.cpp`: `nvlink` enters the protocol priority table at 3
  (below the same-host GPU-IPC group, above `rdma`), gated by
  `nvlinkReachable()`. A cross-rack target on an nvlink-only segment is
  downgraded to `rdma`/`rdma_twosided`/`tcp` *before* the transfer is
  submitted — a fabric handle cannot be imported across NVLink domains, and
  handing the request to nvlink turns that into an opaque import failure
  inside `submitTransfer` instead.
- `master_service.cpp`: soft mode (rack_id set) ranks same-rack segments
  ahead of others via `preferred_segments`; strict mode (`strict_rack=true`)
  excludes rack-external segments so a full rack fails the allocation and
  triggers eviction rather than spilling cross-rack. Two diagnostics make
  misconfiguration visible: `strict_rack` without `rack_id` warns and falls
  back to soft, and an entirely empty rack index is named explicitly because
  the `NO_AVAILABLE_HANDLE` it otherwise produces is indistinguishable from
  real capacity exhaustion.
- `replica_selection.h`: new same-rack MEMORY tier between the local tiers and
  remote MEMORY. Local NOF_SSD still outranks same-rack MEMORY, unchanged.
  Replicas without rack info behave as remote, so an empty `local_rack_id`
  reproduces the previous behaviour exactly.
- `types.h`: `rack_id` is carried as `struct_pack::compatible<std::string, 1>`
  on `Segment` and `AllocatedBuffer::Descriptor` so the struct type hash stays
  stable and a new client can still exchange `MountSegment` with an old master.

**2. Fabric-handle allocation** (env-gated)

`NvlinkTransport::registerLocalMemory()` calls
`cuMemRetainAllocationHandle()`, which fails on a `cudaMalloc` or
`aligned_alloc` pointer — after which registration *returns success having
registered nothing*. The segment mounts, appears in the master's index, gets
selected, and only fails at the first remote read. So the buffer must carry a
fabric handle.

- `MC_STORE_VRAM_FABRIC=1` — device memory as an exportable fabric allocation.
- `MC_STORE_HOST_FABRIC=1` — host DRAM via CUDA VMM with
  `CU_MEM_LOCATION_TYPE_HOST_NUMA` (EGM), new in this PR. NUMA node comes from
  `cudaDevAttrHostNumaId` on the current device; a driver that does not expose
  it fails the allocation rather than guessing node 0. Access descriptors are
  `device_count + 1` — every GPU plus the host NUMA node — because unlike a
  VRAM segment this region is written by the CPU and read by GPUs.

Neither switch falls back to plain memory on failure: a non-fabric pointer only
defers the failure to mount/read time with a far less obvious cause. Both are
value-based rather than presence-based, so `=0` cannot silently opt a node in.

Per the "large changes" rule in `CONTRIBUTING.md`, this is ~1200 LOC excluding
tests and config parsing, so **an RFC issue is likely required** — happy to
file one, or to split this into the rack-awareness half and the fabric half if
reviewers prefer smaller PRs.

## Module

- [x] Transfer Engine (`mooncake-transfer-engine`)
- [x] Mooncake Store (`mooncake-store`)
- [ ] Mooncake Conductor (`mooncake-conductor`)
- [ ] Reshard (`mooncake-reshard`)
- [ ] Mooncake EP (`mooncake-ep`)
- [ ] Mooncake PG (`mooncake-pg`)
- [ ] Integration (`mooncake-integration`)
- [ ] P2P Store (`mooncake-p2p-store`)
- [x] Python Wheel (`mooncake-wheel`)
- [x] Common (`mooncake-common`)
- [ ] Mooncake RL (`mooncake-rl`)
- [ ] CI/CD
- [x] Docs
- [ ] Other

## Type of Change

- [ ] Bug fix
- [x] New feature
- [ ] Refactor
- [ ] Breaking change
- [x] Documentation update
- [ ] Performance improvement
- [ ] Other

## How Has This Been Tested?

Built and tested on a CUDA host (Ubuntu 22.04, CUDA 12.9.86, gcc 11.4.0).
The `USE_CUDA=ON` build matters: a `USE_CUDA=OFF` build compiles only the
non-CUDA stub branch of `host_fabric_allocator.cpp` and none of the call sites,
which is how a missing `MC_STORE_HOST_FABRIC` declaration initially went
unnoticed.

**Test commands:**
```bash
mkdir -p build && cd build
cmake .. -DUSE_CUDA=ON -DBUILD_UNIT_TESTS=ON -DSKBUILD=ON
make host_fabric_config_test vram_fabric_config_test buffer_allocator_test -j64

./mooncake-store/tests/host_fabric_config_test
./mooncake-store/tests/vram_fabric_config_test
./mooncake-store/tests/buffer_allocator_test
```

**Test results:**
```
host_fabric_config_test   [  PASSED  ] 6 tests.
vram_fabric_config_test   [  PASSED  ] 6 tests.
buffer_allocator_test     [  PASSED  ] 21 tests.
```

The full `mooncake_store` library builds with `USE_CUDA=ON`, so the CUDA branch
of the new allocator and both call sites in `client_buffer_allocation.cpp` are
compiled.

- [x] Unit tests pass
- [ ] Integration tests pass (if applicable)
- [ ] Manual testing done (describe below)

**Not verified:** the runtime behaviour of
`cuMemCreate(HOST_NUMA + CU_MEM_HANDLE_TYPE_FABRIC)` on real hardware, and
end-to-end transfer across a multi-node NVLink domain. Both need a topology I
do not have access to; the CUDA path has only been through the compiler. Host
NUMA node selection is likewise unverified on a multi-socket machine.

`multi_transport_locality_test`, `segment_test`, `replica_selection_test`,
`serializer_test`, and `test_mooncake_config.py` were extended but not
re-run in this round.

## Checklist

- [x] I have performed a self-review of my own code
- [x] I have formatted my code using `./scripts/code_format.sh`
- [x] I have run pre-commit on the files changed in this PR and all hooks pass
- [x] I have updated the documentation (if applicable)
- [x] I have added tests to prove my changes are effective
- [ ] For changes >500 LOC: I have filed an RFC issue

Note on pre-commit: all hooks pass on the files this PR changes. `cmake-format`
also rewraps a pre-existing long line in `mooncake-store/tests/CMakeLists.txt`
(`dynamic_replication_lease_table_test`) that is unrelated to this change; that
rewrite was left out to keep the diff scoped.

Documentation is in `ppio/docs/`: `change.md` (per-file change list, including
the vLLM-side pieces that live outside this repo), `design.md` (rationale for
each decision), `build-deploy.md` (dependencies, build flags, deployment
config, verification, troubleshooting).

## AI Assistance Disclosure

- [ ] No AI tools were used
- [x] AI tools were used (specify below)

Written with Claude Code. It wrote the fabric allocators, the routing
predicates, the placement/selection changes, the tests, and the documents under
`ppio/docs/`. The human submitter is responsible for reviewing and defending
every line before merge — in particular the CUDA VMM lifecycle in
`host_fabric_allocator.cpp`, which has been compiled but not exercised on
hardware.
