# Hardware Optimization & Resource Tuning

TeleVault v4.0.3 is written in native C++23, designed to achieve wire-speed throughput on powerful workstations while remaining lightweight enough to run seamlessly on resource-constrained single-board computers (Raspberry Pi, low-end VPS, embedded gateways).

---

## Low-Resource Mode (`--low-resource`)

Enable low-resource mode on any push or pull operation:

```bash
tvt push large_archive.tar.gz --low-resource
tvt pull large_archive.tar.gz --low-resource --resume
```

### Operational Comparison

| Parameter | Default Profile | Low-Resource Profile (`--low-resource`) |
|---|---|---|
| **Chunk Size** | 256 MB | 32 MB |
| **Max Concurrent Uploads** | 8 threads | 2 threads |
| **Max Concurrent Downloads** | 10 threads | 2 threads |
| **RAM Utilization (Peak)** | ~512 MB – 1.5 GB | < 96 MB |
| **Recommended Systems** | Desktops, Dedicated Servers | Raspberry Pi 3/4/5, 512MB RAM VPS |

---

## Performance Tuning Configuration

Tuning options can be customized in `~/.config/televault/config.json`:

```json
{
  "parallel_uploads": 8,
  "parallel_downloads": 10,
  "chunk_size": 268435456,
  "max_retries": 5,
  "retry_delay": 1.0,
  "low_resource": false
}
```

### Concurrency Recommendations

| Deployment Profile | Cores | RAM | Upload Workers | Download Workers |
|---|---|---|---|---|
| **High-Performance Server** | 8+ | 16 GB+ | 12 | 15 |
| **Developer Workstation** | 4–8 | 8–16 GB | 8 | 10 |
| **Cloud VPS (Standard)** | 2 | 2–4 GB | 4 | 6 |
| **Single Board Computer / Pi** | 4 (ARM) | 1–2 GB | 2 | 2 |

---

## Native Multithreading Architecture

TeleVault replaces the Python asyncio event loop with a C++23 native `tv::AsyncExecutor` thread pool:

1. **Streaming Chunker Thread**: Reads chunks sequentially from disk into memory-bounded buffers without buffering the whole file in RAM.
2. **Crypto & Compression Workers**: Run concurrently across all available CPU cores, taking full advantage of SIMD vector instructions (AVX-512, AVX2, ARM NEON).
3. **TDLib MTProto Client**: Dispatches asynchronous chunk transfers directly across multiple network connections to Telegram datacenters.
4. **Resumable Transfers with Exponential Backoff**: Transfer progress is tracked atomically, automatically retrying dropped chunks with jittered backoff without discarding verified progress.
