# Hardware Optimization

TeleVault runs efficiently on any system, including low-RAM machines. This page covers performance tuning and resource-constrained operation.

## Low-Resource Mode

Enable with `--low-resource` on any command:

```bash
tvt push large_file.iso --low-resource
tvt pull large_file.iso --low-resource --resume
```

### What Changes

| Setting | Default | Low-Resource |
|---|---|---|
| Chunk Size | 256 MB | 32 MB |
| Parallel Uploads | 8 | 2 |
| Parallel Downloads | 10 | 2 |

### When to Use

- Systems with **< 2 GB RAM**
- **Raspberry Pi** or similar SBCs
- **VPS** with limited memory
- **Mobile** or embedded devices
- Unstable network connections (fewer concurrent transfers = fewer retries)

### Memory Footprint

Each chunk is held in memory during processing:

- **Default mode**: ~256 MB per chunk × 8 parallel = up to 2 GB peak
- **Low-resource mode**: ~32 MB per chunk × 2 parallel = ~64 MB peak

The actual memory usage is lower because chunks are streamed, not fully buffered. But these are the worst-case bounds.

## Performance Tuning

### Chunk Size

Larger chunks = fewer API calls, higher throughput per chunk, more memory per chunk.

Smaller chunks = more API calls, better resume granularity, less memory per chunk.

The default 256 MB is optimal for most desktop/server systems with good network connections.

### Parallelism

Upload and download concurrency can be tuned in `~/.config/televault/config.json`:

```json
{
  "parallel_uploads": 8,
  "parallel_downloads": 10,
  "chunk_size": 268435456,
  "max_retries": 3,
  "retry_delay": 1.0
}
```

**Guidelines:**

| Scenario | Uploads | Downloads |
|---|---|---|
| Desktop, good network | 8 | 10 |
| Server, high bandwidth | 12 | 15 |
| Low-RAM system | 2 | 2 |
| Unstable network | 4 | 4 |

### Network Speed Tracking

The CLI tracks transfer speed using Exponential Moving Average (EMA) smoothing:

```
⬆️ Uploading photo.jpg (3/8 chunks)  45.2 MB/s  ████████████░░░░░░░░ 65%
```

Speed is updated every chunk with a 0.3 smoothing factor to avoid jitter.

## Async I/O

File hashing uses `aiofiles` with a `ThreadPoolExecutor` to avoid blocking the event loop:

- **Hashing** runs in a background thread pool
- **Chunk reading** uses async file I/O
- **Encryption/compression** runs on the event loop (CPU-bound but fast)

This keeps the upload pipeline saturated — hashing the next chunk while uploading the current one.

## Resumable Operations

Both uploads and downloads support resume:

```bash
# Resume interrupted upload
tvt push large_file.iso --resume

# Resume interrupted download
tvt pull large_file.iso --resume
```

Progress files include CRC32 checksums to detect corruption. If the progress file is corrupted, the operation starts fresh but partial data is preserved.
