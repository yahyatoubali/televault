# Virtualization: FUSE3 Filesystem & WebDAV Server

TeleVault can expose your encrypted vault as a standard local mount point or serve it over HTTP/WebDAV, allowing file managers, media players, and desktop applications to access your files on demand.

---

## 1. Native FUSE3 Mount (`tvt mount`)

Mount your vault directory directly into the Linux filesystem:

```bash
# Read-only mount (safest for browsing)
tvt mount ~/my-vault --read-only

# Read-write mount with custom LRU chunk cache
tvt mount ~/my-vault --cache-size 500 --cache-dir /tmp/tvcache
```

### On-Demand Streaming Architecture

The FUSE3 subsystem downloads only the specific byte ranges and chunks requested by the operating system kernel:

```
Application reads bytes 0-4096 from photo.jpg
    │
    ▼
FUSE getattr → cached metadata (30s TTL)
    │
    ▼
FUSE open → prefetch leading chunk into LRU cache
    │
    ▼
FUSE read → fetch_range(0, 4096)
    │
    ▼
ChunkCache.fetch_chunk(0) → download from Telegram
    │
    ▼
AES-256-GCM Decrypt → Zstandard Decompress → Return 4096 bytes
```

### FUSE Mount Options

| Flag | Default | Description |
|---|---|---|
| `--read-only` | `false` | Mount filesystem as read-only |
| `--cache-size <MB>` | `100` | In-memory LRU chunk cache size in megabytes |
| `--cache-dir <path>` | `~/.local/share/televault/fuse_cache` | Disk directory for partial chunk caching |
| `--allow-other` | `false` | Allow non-root users to access mount point |
| `--foreground` | `true` | Run in foreground (cleanly unmounts on `Ctrl+C`) |

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install -y libfuse3-dev fuse3

# Arch Linux
sudo pacman -S fuse3
```

---

## 2. Native WebDAV Server (`tvt serve`)

Serve your encrypted vault over HTTP/WebDAV for network-attached storage (NAS) access from file managers and mobile clients:

```bash
# Default listener on http://0.0.0.0:8080
tvt serve

# Custom bind address and port
tvt serve --host 127.0.0.1 --port 9090

# Read-only server
tvt serve --read-only
```

### Supported WebDAV Clients

| Platform | Client / Protocol | Connection URL |
|---|---|---|
| **Linux** | GNOME Files / Dolphin (gvfs / kio) | `dav://localhost:8080/` |
| **macOS** | Finder (Connect to Server) | `http://localhost:8080` |
| **Windows** | Map Network Drive | `http://localhost:8080` |
| **Mobile (iOS/Android)** | Documents by Readdle, FE File Explorer | `http://<server-ip>:8080` |

### WebDAV Protocol Operations

- `PROPFIND`: Directory listing and file metadata querying.
- `GET`: On-demand streaming chunk download with HTTP Range header support.
- `HEAD`: File existence and size checks.
- `OPTIONS`: WebDAV Class 1/2 compliance negotiation.
