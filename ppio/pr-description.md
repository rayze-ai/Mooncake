## Description

Lets a Mooncake Store segment living in one node's DRAM be read by GPUs on
other nodes in the same NVLink domain (GB300 NVL72), so the prefix-cache
path P → store → D runs over NVLink instead of RoCE. Scope is a single rack;
no topology awareness.

**Why it did not work before.** `NvlinkTransport::registerLocalMemory()`
retains the allocation handle of the region it registers. On an
`aligned_alloc()` segment that fails, and the transport returned success
having published nothing: the segment mounted, appeared in the master's
index, was selected, and only failed at the first remote read. So the
segment has to be VMM-backed from the start, and `main` had no path that
allocated it that way.

**Three pieces:**

- **`host_fabric_allocator.{h,cpp}`** (new): `cuMemCreate(HOST_NUMA, FABRIC)`,
  reserve, map, and `cuMemSetAccess` for every device plus the host NUMA node
  (the CPU writes this memory, GPUs read it). Fails closed — no fallback to
  plain memory, no guessing the NUMA node — because a non-fabric pointer only
  defers the failure to read time. Gated by `MC_STORE_HOST_FABRIC`, which is
  value-based so `=0` is off and an invalid value warns and stays off.

- **Allocation predicate asks the transfer engine**, not the protocol string.
  Whether an nvlink transport is installed is decided by build flags plus
  `MC_FORCE_MNNVL` / `MC_INTRANODE_NVLINK` / HCA absence, never by the
  protocol a client passes to `setup()` — and the Store does not recognise
  `"nvlink"` as a protocol at all. `TransferEngine::nvlinkUsesFabricMem()`
  forwards down to `NvlinkTransport::usesFabricMem()`; the Store reads it
  once at setup into a process-level flag so allocation and release agree
  on which allocator owns the memory.

- **`registerLocalMemory()` honours `remote_accessible` on the fabric path.**
  A local staging buffer (`remote_accessible=false`) that is not VMM-backed
  is still registered for local use, as before — the Store's `local_buffer`
  depends on this. A remote-accessible region that is not VMM-backed now
  fails registration so `MountSegment` rejects it, unless
  `MC_NVLINK_TOLERATE_NON_FABRIC` is set to accept an unreachable
  registration. Both Store call sites already passed the right value; the
  nvlink branch had been discarding it with `(void)`.

Existing code changes are thin: one getter per TE layer, two mirrored
branches in `client_buffer_allocation.cpp`, one call in `real_client.cpp`
setup, and the three-way check in `nvlink_transport.cpp`. Everything
substantial is in new files.

**`ppio/`** holds the deployment write-up for this work: `design.md` (the
three data paths and which transport stack decides each, why the KV cache
side needs only `--enable-cumem-allocator`), `todo.md` (staged plan with
status), `deployment.md` (config, build flags, per-role deployment,
verification, troubleshooting).

## Module

- [x] Transfer Engine (`mooncake-transfer-engine`)
- [x] Mooncake Store (`mooncake-store`)
- [ ] Mooncake Conductor (`mooncake-conductor`)
- [ ] Reshard (`mooncake-reshard`)
- [ ] Mooncake EP (`mooncake-ep`)
- [ ] Mooncake PG (`mooncake-pg`)
- [ ] Integration (`mooncake-integration`)
- [ ] P2P Store (`mooncake-p2p-store`)
- [ ] Python Wheel (`mooncake-wheel`)
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

Built on a CUDA host (Ubuntu 22.04, x86_64, CUDA 12.9) with the
configuration a deployment actually needs. `-DUSE_MNNVL=ON` matters: it
defaults to OFF, and without it the nvlink transport is not compiled, so
`nvlinkUsesFabricMem()` is always false and the new code has no reachable
path.

**Test commands:**
```bash
mkdir -p build && cd build
cmake .. -DUSE_CUDA=ON -DUSE_MNNVL=ON -DBUILD_UNIT_TESTS=ON
make host_fabric_config_test nvlink_fabric_ready_test buffer_allocator_test -j$(nproc)
./mooncake-store/tests/host_fabric_config_test
./mooncake-store/tests/nvlink_fabric_ready_test
./mooncake-store/tests/buffer_allocator_test
```

**Test results:**
```
host_fabric_config_test    [  PASSED  ] 6 tests.
nvlink_fabric_ready_test   [  PASSED  ] 4 tests.
buffer_allocator_test      [  PASSED  ] 21 tests.
```

- [x] Unit tests pass
- [ ] Integration tests pass (if applicable)
- [ ] Manual testing done (describe below)

**Not verified:** `cuMemCreate(HOST_NUMA + FABRIC)` on real hardware — the
allocator's CUDA branch has been through the compiler only. On the target
GB300 nodes `cuMemCreate(DEVICE, FABRIC)` and `cuMemExportToShareableHandle`
both return success and `nvidia-smi -q` reports `Fabric State: Completed`,
so the fabric itself is up; the HOST_NUMA variant is the untested part.
End-to-end P → store → D over NVLink also not yet run. The build is x86; an
arm64 build for GB300 (Grace) has not been done.

## Checklist

- [x] I have performed a self-review of my own code
- [x] I have formatted my code using `./scripts/code_format.sh`
- [x] I have run pre-commit on the files changed in this PR and all hooks pass
- [x] I have updated the documentation (if applicable)
- [x] I have added tests to prove my changes are effective
- [ ] For changes >500 LOC: I have filed an RFC issue

Note on pre-commit: all hooks pass on the changed files. `cmake-format`
also wants to rewrap a pre-existing long line in
`mooncake-store/tests/CMakeLists.txt` (`dynamic_replication_lease_table_test`)
unrelated to this change; that rewrite is left out to keep the diff scoped.

## AI Assistance Disclosure

- [ ] No AI tools were used
- [x] AI tools were used (specify below)

Written with Claude Code: the allocator, the predicate chain, the
`remote_accessible` check, the tests, and the `ppio/` documents. The human
submitter is responsible for reviewing and defending every line — in
particular the CUDA VMM lifecycle in `host_fabric_allocator.cpp`, which has
been compiled but not exercised on hardware.
