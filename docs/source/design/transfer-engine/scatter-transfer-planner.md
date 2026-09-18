# Scatter Transfer Planning

Scatter workloads span two different regimes. A reshard operation may contain
a few large ranges, while an embedding lookup may contain tens of thousands of
small rows. Total bytes alone cannot select the best transfer path: the former
is normally bandwidth-limited and the latter can be limited by WQE posting and
completion rates.

The planner provides one cost model for Store, Engram, resharding, and
Structured Object callers. It does not branch on a NIC model, token count, or
a fixed fragment threshold.

## Workload normalization

The planner first coalesces fragments whose local and remote addresses are both
adjacent. It then forms legal direct work requests using the runtime
`max_send_sge` and maximum-message limits. Work requests are retained in
power-of-two payload buckets; a single average payload would hide the cost of a
skewed mix of small and large requests.

For a bucket `b`, let `W_b` be its work-request count. For each candidate lane
count `L`, the effective byte ceiling is measured as
`B_eff(L) = min(B_link, B_PCIe, B_source, B_destination, B_observed(L))`.
The direct RDMA time is bounded by:

```text
T_direct(L) = startup + planning + max(
    bytes / B_eff(L),
    sum_b W_b / M_wqe(b, L),
    ceil(W / k_signal) / M_cqe(L),
    max(RTT, W * RTT / (L * queue_depth))) + tail
```

`M_wqe(b, L)` and `M_cqe(L)` are measured service rates. They are not constants
copied from one device and are not assumed to scale linearly with `L`.

When a gather implementation is available, its estimate is:

```text
G = max(memory_traffic / B_memory,
        fragment_count / M_copy)
R_bulk = the same RDMA model applied to packed work requests
```

For `P` pipeline chunks, ideal double-buffered completion is:

```text
T_gather(L, P) = startup + control + planning
                 + G/P + R_bulk(L)/P
                 + (P - 1) * max(G/P, R_bulk(L)/P)
                 + P * chunk_overhead + tail
```

Only chunks at least as large as both the per-lane bandwidth-delay product and
the measured WQE saturation payload are considered. A logical stage may
contain multiple RDMA messages when it exceeds the runtime maximum message
size.

The selected plan is the minimum-cost candidate over the installed execution
paths, available lane counts, and legal pipeline depths:

```text
plan = argmin T(path, L, P)
       path in {direct, gather}
       L in measured lane configurations
       P in legal pipeline depths
```

This candidate search is important. More QPs can improve a WQE-bound direct
path while making a small bulk transfer slower. More pipeline chunks improve
overlap only until chunk overhead and tail latency dominate.

## Directional execution

The decision model is common, but path availability is directional:

- A scatter WRITE can gather local fragments into registered staging memory
  and issue packed remote WRITEs.
- A READ from a contiguous remote envelope can issue a packed READ and scatter
  locally.
- A READ from unrelated remote addresses, such as random embedding rows,
  requires an owner-side active gather service. Requester-side staging alone
  does not reduce the number of RDMA READ work requests.

Callers must consider gather only after the corresponding execution path and
registered staging lifetime are established. Otherwise they keep the direct
path.

## Runtime calibration

Calibration is keyed by peer, direction, memory type, lane count, and payload
bucket. It maintains observations for single-bucket WQE service rate, copy byte
and operation rates, planning rate, control latency, and chunk overhead. Byte
and aggregate WQE capacities use observed upper envelopes, because a
small-WQE-limited sample must not lower a previously measured link ceiling.
Mixed-payload samples update only the aggregate fallback: they cannot identify
the individual bucket rates. Payload buckets remain separate so observations
from large reshard ranges do not incorrectly tune small Engram rows.

A five-percent hysteresis band is used by default. A caller already using
gather keeps it until the direct estimate wins outside the band; a direct
caller switches only when gather wins outside the band. This avoids oscillation
around the measured crossover.

## Validation expectations

Validation must use paired direct/gather runs over the same connections, QPs,
registered memory, row IDs, and iteration window. It must include:

- fixed bytes with few large and many small fragments
- uniform and skewed payload distributions
- runtime `max_send_sge` and maximum-message limits
- multiple lane and queue-depth candidates
- byte-for-byte output verification
- held-out shapes that are not used to fit calibration

The planner predicts a path; it does not replace end-to-end profiling. Control
RPC, copy, posting, completion, and tail time must remain separately visible.

## ERDMA validation

The model was checked on two A10 + ERDMA peers with paired direct/gather runs.
Both paths used the same connections, row IDs, destination, warmup window, and
iteration window. The adapter exposed one SGE per RDMA operation. Every case
verified the destination byte for byte.

Keeping the payload near 1 MiB while changing only the fragmentation produced
the expected crossover:

| Ranges | Bytes per range | Direct p50 | Gather p50 | Winner |
|---:|---:|---:|---:|---|
| 1 | 1 MiB | 750.98 us | 822.68 us | direct |
| 4 | 256 KiB | 758.10 us | 810.03 us | direct |
| 64 | 16 KiB | 249.16 us | 284.88 us | direct |
| 1,024 | 1 KiB | 1,902.41 us | 289.22 us | gather |
| 16,384 | 64 B | 28,428.60 us | 374.53 us | gather |

The PR4083-shaped workload uses two layers, 24 tables per layer, and 264-byte
rows. With three QPs, 2,048 tokens contain 98,304 ranges and 25,952,256 bytes:

| Path | Pipeline chunks | p50 | p95 | Useful throughput |
|---|---:|---:|---:|---:|
| Direct | 1 | 69.88 ms | 73.69 ms | 2.97 Gbit/s |
| Gather | 1 | 4.85 ms | 5.46 ms | 42.80 Gbit/s |
| Gather | 2 | 3.78 ms | 4.16 ms | 54.93 Gbit/s |
| Gather | 4 | 3.23 ms | 3.59 ms | 64.24 Gbit/s |
| Gather | 8 | 3.21 ms | 4.02 ms | 64.76 Gbit/s |

At four chunks, gathering took 2.19 ms and the RDMA window took 2.44 ms, but
their overlapped owner time was 3.16 ms instead of their 4.63 ms sum. Eight
chunks did not materially improve the median and made p95 worse, so four is the
measured choice for this shape.

The timed RDMA window starts at the first post and ends at the final CQ
completion. Later gather chunks execute during that interval, so this is an
overlapped completion window rather than isolated NIC time. It moved 25.95 MB
in 2.44 ms, or about 85.2 Gbit/s of useful payload.

Native `ib_read_bw` on the same single ERDMA device used 8 MiB messages, 128
outstanding reads, and the device's 1,024-byte MTU:

| QPs | Average bandwidth |
|---:|---:|
| 1 | 54.12 Gbit/s |
| 3 | 95.45 Gbit/s |
| 8 | 95.46 Gbit/s |

Three QPs therefore saturate the single-device path for large reads. The
85.2 Gbit/s overlapped window reaches 89.3% of the measured 95.46 Gbit/s native
READ ceiling. Its remaining gap includes short-transfer, chunk-boundary, and
control effects; the larger end-to-end gap additionally includes CPU gathering.

These measurements validate the candidate model, not a universal numeric
threshold. A different NIC or memory type supplies different measured
`B_eff`, WQE/CQE rates, copy rates, and control costs to the same equations.

## Implementation boundary

The existing direct scatter path can consume this policy without a protocol
change. Random Engram reads cannot: they require an owner-side active-gather
executor, compact or fragmented range-plan messages, bounded pre-registered
staging, and completion/error fallback. That executor must be capability
negotiated and keep direct scatter as the compatibility path. The cost model
must not advertise gather until that executor is installed.
