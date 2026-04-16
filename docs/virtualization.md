# Virtualization

Mount your TeleVault as a local filesystem or serve it over HTTP. Access your encrypted files from any application without using the CLI.

## FUSE Mount

Mount your vault as a read-only or read-write filesystem:

```bash
pipx install televault[fuse]

# Read-only mount
tvt mount ~/vault --read-only

# Read-write with custom cache
tvt mount ~/vault --cache-size 500 --cache-dir /tmp/tvcache
```

### How It Works

The FUSE driver uses **on-demand chunk streaming** — only the chunks needed for a specific read are downloaded, not the entire file.

```
Application reads bytes 0-4096 from file.jpg
    │
    ▼
FUSE getattr → cached metadata (30s TTL)
    │
    ▼
FUSE open → prefetch first 3 chunks into LRU cache
    │
    ▼
FUSE read → fetch_range(0, 4096)
    │
    ▼
ChunkCache.fetch_chunk(0) → download from Telegram
    │
    ▼
Decrypt → Decompress → Return bytes 0-4096
```

### Architecture

| Component | Role |
|---|---|
| `TeleVaultFuse` | FUSE operations implementation |
| `ChunkCache` | Per-file chunk manager, fetches only needed chunks |
| `LRUCache` | Global chunk data cache, configurable size (default 100 MB) |
| Index refresh | 30s TTL on `getattr`, forced refresh on `readdir` |
| Prefetch | First 3 chunks loaded on file open |

### Options

| Flag | Default | Description |
|---|---|---|
| `--read-only` | false | Mount as read-only |
| `--cache-size` | 100 | LRU cache size in MB |
| `--cache-dir` | `~/.local/share/televault/fuse_cache` | Local cache directory |
| `--allow-other` | false | Allow other users to access the mount |
| `--foreground` | true | Run in foreground (Ctrl+C to unmount) |
| `--background` | false | Run as daemon |

### Performance

- **First browse**: Index is preloaded before the mount becomes active, so the first directory listing is instant
- **Sequential reads**: Prefetching keeps the next chunks in cache
- **Random access**: Only the overlapping chunks are downloaded
- **Large files**: On-demand streaming means a 2 GB file doesn't need to be fully downloaded to read the first 1 KB

### Requirements

```bash
# Linux
sudo apt install fuse libfuse2

# macOS
# Install macFUSE from https://macfuse.io/
```

## WebDAV Server

Serve your vault over HTTP/WebDAV for access from file managers, mobile apps, or any WebDAV client:

```bash
pipx install televault[webdav]

# Default: http://0.0.0.0:8080
tvt serve

# Custom host/port
tvt serve --host 192.168.1.100 --port 9090

# Read-only
tvt serve --read-only
```

### Accessing from Clients

| Client | URL |
|---|---|
| **macOS Finder** | Go → Connect to Server → `http://localhost:8080` |
| **Windows Explorer** | Map Network Drive → `http://localhost:8080` |
| **Linux (gvfs)** | `dav://localhost:8080/` in file manager |
| **Mobile (iOS/Android)** | Any WebDAV client app |

### Architecture

The WebDAV server is built on `aiohttp` and implements:

- **PROPFIND** — List files and directories
- **GET** — Download files (streaming, on-demand)
- **HEAD** — File metadata
- **OPTIONS** — WebDAV capability discovery
- **Read-only mode** — PUT, DELETE, MKCOL, PROPPATCH return 403

Files are streamed on-demand — only the chunks needed for a specific read are fetched from Telegram.

### Options

| Flag | Default | Description |
|---|---|---|
| `--host` | `0.0.0.0` | Bind address |
| `--port` | 8080 | Port number |
| `--read-only` | false | Read-only mode |
| `--cache-dir` | `~/.local/share/televault/webdav_cache` | Local cache directory |
