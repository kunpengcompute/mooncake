# `store_kv_bench.py`

`store_kv_bench.py` is a Mooncake Store end-to-end KV benchmark. It uses the
Python Store binding to exercise ordinary object APIs and registered-buffer
APIs against a real Mooncake cluster.

The registered-buffer path supports:

- Host DMA memory and CUDA device memory
- One buffer or multiple slices per object
- Block-aligned and non-aligned logical object sizes
- Deliberately unaligned buffer addresses
- Contiguous and non-contiguous SGL layouts
- Read-after-write data verification
- Logical and estimated NoF physical throughput reporting

## Scope

The benchmark supports these object-semantic scenarios:

- `verify_write`: fixed-count write followed by complete readback verification
- `fill`: fixed-count dataset fill
- `write_perf`: time-based or fixed-count write benchmark
- `read_perf`: optional prepare-write phase followed by a read benchmark
- `mixed_rw`: reads from prepared objects mixed with writes to fresh keys

It does not configure an SPDK target, mount a NoF segment, inject faults, or
perform range reads. Those operations must be completed before the relevant
benchmark phase starts. Phase gaps are available when an external operation is
required between phases.

## Remote NoF topology

A typical GPU-to-SSD setup is:

```text
GPU / initiator host                       SSD / target host
--------------------                      -----------------
store_kv_bench.py
Mooncake Store client
SPDK NVMe-oF initiator  -- RDMA network -> SPDK NVMf target
Host or CUDA buffers                       SSD namespace
```

The SSD namespace must already be exported by the SPDK NVMf target and mounted
in Mooncake as a NoF segment. The benchmark does not take the target address
directly; Mooncake Master returns the selected segment's `transport_endpoint`
in the replica descriptor.

For an isolated NoF test, use:

```text
--memory-replica-num 0
--nof-replica-num 1
```

If the cluster contains multiple NoF segments, use `--preferred-segment` to
force the intended target.

## API and buffer modes

### Plain API

`--io-api=plain` uses Python byte buffers:

- Single object: `put`, `get`
- Batch: `put_batch`, `get_batch`

Plain mode only supports `--memory-kind=host` and
`--buffer-layout=single`.

### Registered-buffer API

`--io-api=zcopy` allocates one pool, registers the complete allocation once,
and passes addresses inside that allocation to Mooncake.

With `--buffer-layout=single`, it uses:

- Single object: `put_from`, `get_into`
- Batch: `batch_put_from`, `batch_get_into`

With `--buffer-layout=multi`, it uses:

- `batch_put_from_multi_buffers`
- `batch_get_into_multi_buffers`

Multi layout is used even when `--batch-size=1`, because the single-object APIs
accept only one buffer.

## Memory kinds

### Host

```text
--io-api zcopy --memory-kind host
```

The benchmark obtains DMA-capable Host memory from Mooncake's exported
allocation helper and registers the complete pool with `register_buffer`.

### CUDA

```text
--io-api zcopy --memory-kind cuda --cuda-device 0
```

CUDA mode requires PyTorch, a CUDA-enabled Mooncake build, SPDK GPU dma-buf
support, and a GPU/RNIC topology capable of GPUDirect RDMA. The benchmark sets
the selected CUDA device before `store.setup()`, so the SPDK GPU memory domain
and scratch pool are initialized for the correct device.

Recommended environment:

```bash
export MC_SPDK_GPU_DMABUF=1
unset MC_SPDK_GPU_DMABUF_DEVICE_ID
```

Do not globally force `MC_SPDK_GPU_DMABUF_DEVICE_ID=0` when different
processes test different GPUs.

CUDA buffer fills and clears are synchronized before Mooncake receives the
pointer. The CUDA tensor remains alive until after `unregister_buffer`, so the
registered allocation cannot be recycled by the PyTorch allocator while I/O
is active.

## Slice layout controls

### Balanced slices

```text
--buffer-layout multi --segments 3
```

The logical object is divided as evenly as possible. For `--value-size=10`,
three slices become `[4, 3, 3]`.

### Explicit slices

```text
--buffer-layout multi --segment-sizes 1000,2000,1097
```

Every size must be positive, at least two sizes must be supplied, and their
sum must equal `--value-size`.

### Non-contiguous slices

```text
--segment-gap 64
```

This leaves 64 unused bytes between adjacent slices. The gap is part of the
registered allocation but not part of the logical object. It ensures the test
uses genuinely non-contiguous SGE addresses.

### Unaligned addresses

```text
--buffer-offset 1
```

This starts each object's first slice one byte after its slot base. It is
independent of logical-length alignment and is useful for verifying that the
initiator does not impose an unnecessary DWORD-alignment rule on every SGE.

All slices in one request use the same memory domain. The benchmark does not
construct mixed Host/GPU SGLs, because Mooncake intentionally rejects them.

## Logical and physical alignment

`--value-size` is the logical object size and may be any positive integer.
There is no longer a 512-byte write restriction in the benchmark.

`--block-size` is used only to estimate physical NoF bytes and padding. It does
not configure the target and must match the real namespace sector size.

For a 4096-byte namespace block:

```text
value-size=4096 -> physical=4096, padding=0
value-size=4095 -> physical=4096, padding=1
value-size=4097 -> physical=8192, padding=4095
```

The estimate is reported for NoF-only workloads. Mixed memory/NoF replica
workloads report logical bytes because the benchmark cannot determine which
replica served every read. NoF write estimates include the requested number of
NoF replicas.

## Key rules

- Keys are deterministic: `{padded/truncated prefix}{16-digit object id}`.
- The same prefix, key size, and object-id start produce the same key sequence.
- `--verify` requires `--pattern`.
- `memory-replica-num` and `nof-replica-num` cannot both be zero.
- `prepare-objects=0` reuses `nr-objects`.
- Use a fresh key prefix when comparing different object sizes.

## Functional examples

The addresses below are placeholders:

- `GPU_RDMA_IP`: initiator-side address advertised by Mooncake
- `MASTER_IP`: Mooncake metadata and master services
- `nof-segment-name`: mounted NoF segment for the SSD target

### Host, single buffer, non-aligned object

```bash
export MC_SPDK_GPU_DMABUF=0

python3 mooncake-store/benchmarks/store_kv_bench.py \
  --scenario verify_write \
  --io-api zcopy \
  --memory-kind host \
  --buffer-layout single \
  --value-size 4097 \
  --block-size 4096 \
  --nr-objects 128 \
  --batch-size 8 \
  --numjobs 2 \
  --iodepth 2 \
  --memory-replica-num 0 \
  --nof-replica-num 1 \
  --preferred-segment nof-segment-name \
  --verify \
  --pattern 0x5a \
  --protocol rdma \
  --device-name mlx5_0 \
  --global-segment-size 0 \
  --local-buffer-size $((64*1024*1024)) \
  --local-hostname GPU_RDMA_IP:50071 \
  --metadata-server http://MASTER_IP:8080/metadata \
  --master-server MASTER_IP:50051
```

For a 4096-byte target block, each object submits 4097 logical bytes plus 4095
bytes of Host scratch padding.

### Host, non-contiguous multi-slice object

```bash
python3 mooncake-store/benchmarks/store_kv_bench.py \
  --scenario verify_write \
  --io-api zcopy \
  --memory-kind host \
  --buffer-layout multi \
  --segment-sizes 1000,2000,1097 \
  --segment-gap 64 \
  --buffer-offset 1 \
  --value-size 4097 \
  --block-size 4096 \
  --nr-objects 128 \
  --batch-size 8 \
  --memory-replica-num 0 \
  --nof-replica-num 1 \
  --preferred-segment nof-segment-name \
  --verify \
  --pattern 0xa5 \
  --protocol rdma \
  --device-name mlx5_0 \
  --global-segment-size 0 \
  --local-buffer-size $((64*1024*1024)) \
  --local-hostname GPU_RDMA_IP:50071 \
  --metadata-server http://MASTER_IP:8080/metadata \
  --master-server MASTER_IP:50051
```

The logical NoF SGL is:

```text
Host slice 0: 1000 bytes
Host slice 1: 2000 bytes
Host slice 2: 1097 bytes
Host scratch: 4095 bytes
```

### CUDA, non-contiguous multi-slice object

```bash
export MC_SPDK_GPU_DMABUF=1
unset MC_SPDK_GPU_DMABUF_DEVICE_ID

python3 mooncake-store/benchmarks/store_kv_bench.py \
  --scenario verify_write \
  --io-api zcopy \
  --memory-kind cuda \
  --cuda-device 0 \
  --buffer-layout multi \
  --segment-sizes 1000,2000,1097 \
  --segment-gap 64 \
  --buffer-offset 1 \
  --value-size 4097 \
  --block-size 4096 \
  --nr-objects 128 \
  --batch-size 8 \
  --memory-replica-num 0 \
  --nof-replica-num 1 \
  --preferred-segment nof-segment-name \
  --verify \
  --pattern 0x3c \
  --protocol rdma \
  --device-name mlx5_0 \
  --global-segment-size 0 \
  --local-buffer-size $((64*1024*1024)) \
  --local-hostname GPU_RDMA_IP:50071 \
  --metadata-server http://MASTER_IP:8080/metadata \
  --master-server MASTER_IP:50051
```

The payload slices and tail scratch are all CUDA memory. Host and CUDA memory
are never mixed in the same request.

## Performance comparison

For a 4096-byte namespace block, these sizes answer different questions:

```text
4096 vs 4095: same physical I/O size; isolates scratch/SGL path overhead
4096 vs 4097: crosses a block boundary; measures real I/O amplification
8192 vs 8191: larger same-physical-size comparison
```

Example read benchmark:

```bash
for value_size in 4096 4095 4097; do
  python3 mooncake-store/benchmarks/store_kv_bench.py \
    --scenario read_perf \
    --prepare-mode write \
    --io-api zcopy \
    --memory-kind host \
    --buffer-layout single \
    --value-size ${value_size} \
    --block-size 4096 \
    --prepare-objects 10000 \
    --runtime 60 \
    --batch-size 8 \
    --numjobs 2 \
    --iodepth 4 \
    --key-prefix perf-${value_size} \
    --memory-replica-num 0 \
    --nof-replica-num 1 \
    --preferred-segment nof-segment-name \
    --protocol rdma \
    --device-name mlx5_0 \
    --global-segment-size 0 \
    --local-buffer-size $((64*1024*1024)) \
    --local-hostname GPU_RDMA_IP:50071 \
    --metadata-server http://MASTER_IP:8080/metadata \
    --master-server MASTER_IP:50051
done
```

Use the `read_perf` phase rather than `overall summary` for the comparison,
because the overall result also includes the prepare-write phase.

For pure performance tests, omit `--verify`. Verification copies data back to
Python, and CUDA verification additionally performs a device-to-Host copy.
The benchmark remains an end-to-end Mooncake measurement: key construction,
buffer preparation, Python binding, Master operations, Mooncake scheduling,
RDMA, SPDK, and SSD behavior may all contribute to the result.

`iodepth` is implemented as a lane multiplier:

```text
lanes = numjobs * iodepth
```

Each lane is a thread issuing one blocking Store call at a time. The Python
binding releases the GIL while Store operations execute, so lanes can enter
Mooncake concurrently, but this is not the same as a native asynchronous queue
depth per job.

## Phase gaps

- `--phase-gap-mode=none`: continue immediately
- `--phase-gap-mode=sleep --phase-gap-sec=N`: sleep before the next phase
- `--phase-gap-mode=manual`: wait for Enter
- `--phase-gap-mode=file --phase-gap-file=/tmp/bench.ready`: wait for a marker

If file mode is used, remove an old marker before starting the benchmark.

## Output

Each phase reports:

- Request and KV counts
- Miss and verification-failure counts
- Logical bytes and logical `MiB/s`
- Estimated NoF physical bytes and physical `MiB/s`
- Tail padding bytes and estimated I/O amplification
- Duration, `req/s`, and `kv/s`
- Mean, P50, P95, and P99 request latency
- Aggregated error counts

An overall summary is printed after all phases complete.
