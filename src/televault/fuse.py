"""FUSE filesystem driver for TeleVault - mount your vault as a local directory.

Supports on-demand chunk fetching: only the chunks needed for a read are downloaded,
not the entire file. An LRU cache manages downloaded chunks in memory with
configurable size limits.
"""

import asyncio
import logging
import os
import stat
import sys
import threading
import time
from collections import OrderedDict
from pathlib import Path

from .config import Config, get_data_dir
from .models import FileMetadata
from .telegram import TelegramConfig

logger = logging.getLogger("televault.fuse")

FUSE_AVAILABLE = False
try:
    from fuse import FUSE, FuseOSError
    from fuse import Operations as FuseOperations

    FUSE_AVAILABLE = True
except ImportError:
    pass


class LRUCache:
    """Least Recently Used cache for file chunk data."""

    def __init__(self, max_size_mb: int = 100):
        self._max_size = max_size_mb * 1024 * 1024
        self._cache: OrderedDict[str, bytes] = OrderedDict()
        self._current_size = 0

    def get(self, key: str) -> bytes | None:
        if key in self._cache:
            self._cache.move_to_end(key)
            return self._cache[key]
        return None

    def put(self, key: str, value: bytes) -> None:
        if key in self._cache:
            self._cache.move_to_end(key)
            old_size = len(self._cache[key])
            self._current_size -= old_size
            self._cache[key] = value
            self._current_size += len(value)
        else:
            while self._current_size + len(value) > self._max_size and self._cache:
                evicted_key, evicted_value = self._cache.popitem(last=False)
                self._current_size -= len(evicted_value)
            self._cache[key] = value
            self._current_size += len(value)

    def has(self, key: str) -> bool:
        return key in self._cache

    def remove(self, key: str) -> bool:
        if key in self._cache:
            self._current_size -= len(self._cache[key])
            del self._cache[key]
            return True
        return False

    def clear(self) -> None:
        self._cache.clear()
        self._current_size = 0

    @property
    def size_mb(self) -> float:
        return self._current_size / (1024 * 1024)


class ChunkCacheError(Exception):
    """Error raised by ChunkCache for missing or corrupted chunks."""

    def __init__(self, errno: int, msg: str = ""):
        self.errno = errno
        super().__init__(msg)


class ChunkCache:
    """Manages on-demand chunk fetching and caching for a single file."""

    def __init__(
        self,
        metadata: FileMetadata,
        vault,
        lru_cache: LRUCache,
        password: str | None = None,
    ):
        self.metadata = metadata
        self._vault = vault
        self._lru = lru_cache
        self._password = password
        self._lock = asyncio.Lock()

    def _cache_key(self, chunk_index: int) -> str:
        return f"{self.metadata.id}:{chunk_index}"

    async def fetch_chunk(self, chunk_index: int) -> bytes:
        """Fetch a chunk, using cache if available."""
        key = self._cache_key(chunk_index)

        cached = self._lru.get(key)
        if cached is not None:
            return cached

        async with self._lock:
            cached = self._lru.get(key)
            if cached is not None:
                return cached

            chunk_info = None
            for c in self.metadata.chunks:
                if c.index == chunk_index:
                    chunk_info = c
                    break

            if chunk_info is None:
                raise ChunkCacheError(2, f"Chunk {chunk_index} not found")

            from .chunker import hash_data
            from .compress import decompress_data
            from .crypto import decrypt_chunk

            data = await self._vault.telegram.download_chunk(chunk_info.message_id)

            if hash_data(data) != chunk_info.hash:
                raise ChunkCacheError(5, f"Hash mismatch for chunk {chunk_index}")

            if self.metadata.encrypted and self._password:
                data = decrypt_chunk(data, self._password)

            if self.metadata.compressed:
                data = decompress_data(data)

            self._lru.put(key, data)
            return data

    async def fetch_range(self, offset: int, size: int) -> bytes:
        """Fetch a byte range from the file, downloading only needed chunks."""
        chunk_size = self._vault.config.chunk_size
        result = bytearray()

        first_chunk = offset // chunk_size
        last_chunk = min((offset + size - 1) // chunk_size, len(self.metadata.chunks) - 1)

        for chunk_idx in range(first_chunk, last_chunk + 1):
            chunk_data = await self.fetch_chunk(chunk_idx)

            chunk_start = chunk_idx * chunk_size

            read_start = max(0, offset - chunk_start)
            read_end = min(len(chunk_data), offset + size - chunk_start)

            if read_start < read_end:
                result.extend(chunk_data[read_start:read_end])

            if len(result) >= size:
                break

        return bytes(result[:size])

    def invalidate(self) -> None:
        """Remove all chunks for this file from the LRU cache."""
        for chunk_info in self.metadata.chunks:
            key = self._cache_key(chunk_info.index)
            self._lru.remove(key)

    async def prefetch(self, chunk_indices: list[int] | None = None) -> None:
        """Prefetch chunks in parallel. If None, prefetch first 3 chunks."""
        if chunk_indices is None:
            chunk_indices = list(range(min(3, len(self.metadata.chunks))))

        tasks = []
        for idx in chunk_indices:
            tasks.append(self.fetch_chunk(idx))

        results = await asyncio.gather(*tasks, return_exceptions=True)
        for i, r in enumerate(results):
            if isinstance(r, Exception):
                logger.debug(f"Prefetch failed for chunk {chunk_indices[i]}: {r}")


class TeleVaultFuse(FuseOperations if FUSE_AVAILABLE else object):
    """FUSE filesystem that maps TeleVault storage to a local mount point.

    Uses on-demand chunk fetching: only the chunks needed for a specific read
    are downloaded, not the entire file. An LRU cache keeps recently accessed
    chunks in memory for fast re-reads.
    """

    def __init__(
        self,
        config: Config | None = None,
        telegram_config: TelegramConfig | None = None,
        password: str | None = None,
        cache_dir: str | None = None,
        read_only: bool = False,
        cache_size_mb: int = 100,
    ):
        if not FUSE_AVAILABLE:
            raise ImportError(
                "fusepy is required for FUSE mount. Install with: pipx install televault[fuse]"
            )

        self.config = config or Config.load_or_create()
        self.password = password
        self.read_only = read_only
        self._vault = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._loop_lock = threading.Lock()
        self._telegram_config = telegram_config

        cache_path = Path(cache_dir) if cache_dir else get_data_dir() / "fuse_cache"
        self.cache_dir = cache_path
        self.cache_dir.mkdir(parents=True, exist_ok=True)

        self._lru = LRUCache(max_size_mb=cache_size_mb)
        self._chunk_caches: dict[str, ChunkCache] = {}
        self._file_cache: dict[str, FileMetadata] = {}
        self._path_to_id: dict[str, str] = {}
        self._id_to_path: dict[str, str] = {}
        self._fd = 0
        self._open_files: dict[int, str] = {}
        self._write_buffer: dict[int, bytes] = {}
        self._last_refresh = 0.0
        self._cache_lock = asyncio.Lock()

    def _run_async(self, coro):
        with self._loop_lock:
            if self._loop is None or self._loop.is_closed():
                self._loop = asyncio.new_event_loop()
            loop = self._loop

        try:
            return loop.run_until_complete(coro)
        except RuntimeError:
            new_loop = asyncio.new_event_loop()
            with self._loop_lock:
                self._loop = new_loop
            return new_loop.run_until_complete(coro)

    async def _ensure_connected(self):
        from .core import TeleVault

        if self._vault is None:
            self._vault = TeleVault(
                config=self.config,
                telegram_config=self._telegram_config,
                password=self.password,
            )
            await self._vault.connect()

    async def _refresh_index(self, force=False):
        await self._ensure_connected()
        now = time.time()
        if not force and now - self._last_refresh < 30.0:
            return

        index = await self._vault.telegram.get_index()

        new_ids = set(index.files.keys())
        unknown_ids = [fid for fid in new_ids if fid not in self._file_cache]

        if unknown_ids:
            tasks = []
            for file_id in unknown_ids:
                msg_id = index.files[file_id]
                tasks.append(self._vault.telegram.get_metadata(msg_id))
            results = await asyncio.gather(*tasks, return_exceptions=True)

        async with self._cache_lock:
            if unknown_ids:
                for i, file_id in enumerate(unknown_ids):
                    r = results[i]
                    if isinstance(r, Exception):
                        continue
                    meta = r
                    meta.message_id = index.files[file_id]
                    self._file_cache[file_id] = meta
                    self._path_to_id[f"/{meta.name}"] = file_id
                    self._id_to_path[file_id] = f"/{meta.name}"

            removed = set(self._file_cache.keys()) - new_ids
            for fid in removed:
                meta = self._file_cache.pop(fid)
                path = self._id_to_path.pop(fid, None)
                if path:
                    self._path_to_id.pop(path, None)
                self._chunk_caches.pop(fid, None)

            self._last_refresh = now

    async def _get_chunk_cache(self, file_id: str) -> ChunkCache:
        if file_id not in self._chunk_caches:
            meta = self._file_cache.get(file_id)
            if meta is None:
                raise FuseOSError(2)
            await self._ensure_connected()
            self._chunk_caches[file_id] = ChunkCache(
                metadata=meta,
                vault=self._vault,
                lru_cache=self._lru,
                password=self.password,
            )
        return self._chunk_caches[file_id]

    def _get_stat(self, is_dir=False, size=0, mtime=None):
        now = mtime or time.time()
        st = {
            "st_mode": (stat.S_IFDIR | 0o755) if is_dir else (stat.S_IFREG | 0o644),
            "st_nlink": 2 if is_dir else 1,
            "st_size": size,
            "st_uid": os.getuid(),
            "st_gid": os.getgid(),
            "st_atime": now,
            "st_mtime": now,
            "st_ctime": now,
        }
        if not is_dir:
            st["st_mode"] = stat.S_IFREG | 0o644 if not self.read_only else stat.S_IFREG | 0o444
        return st

    def getattr(self, path, fh=None):
        if path == "/":
            return self._get_stat(is_dir=True)

        if path in self._path_to_id:
            file_id = self._path_to_id[path]
            if file_id in self._file_cache:
                meta = self._file_cache[file_id]
                return self._get_stat(is_dir=False, size=meta.size, mtime=meta.created_at)

        self._run_async(self._refresh_index())

        if path in self._path_to_id:
            file_id = self._path_to_id[path]
            if file_id in self._file_cache:
                meta = self._file_cache[file_id]
                return self._get_stat(is_dir=False, size=meta.size, mtime=meta.created_at)

        raise FuseOSError(2)

    def readdir(self, path, fh):
        entries = [".", ".."]

        self._run_async(self._refresh_index(force=True))

        for vault_path in self._path_to_id:
            parent = str(Path(vault_path).parent)
            if parent == path:
                entries.append(Path(vault_path).name)

        return entries

    def open(self, path, flags):
        file_id = self._path_to_id.get(path)
        if not file_id:
            raise FuseOSError(2)

        self._fd += 1
        fd = self._fd
        self._open_files[fd] = file_id

        chunk_cache = self._run_async(self._get_chunk_cache(file_id))

        try:
            self._run_async(chunk_cache.prefetch())
        except Exception as e:
            logger.debug(f"Prefetch failed for {path}: {e}")

        return fd

    def read(self, path, size, offset, fh):
        file_id = self._open_files.get(fh)
        if file_id is None:
            file_id = self._path_to_id.get(path)
        if file_id is None:
            raise FuseOSError(2)

        meta = self._file_cache.get(file_id)
        if meta is None:
            raise FuseOSError(2)

        if offset >= meta.size:
            return b""

        read_size = min(size, meta.size - offset)

        try:
            chunk_cache = self._run_async(self._get_chunk_cache(file_id))
            data = self._run_async(chunk_cache.fetch_range(offset, read_size))
            return data
        except ChunkCacheError as e:
            raise FuseOSError(e.errno) from None
        except Exception as e:
            logger.error(f"Read failed for {path} at offset {offset}: {e}")

            local_path = self.cache_dir / path.lstrip("/")
            if local_path.exists():
                with open(local_path, "rb") as f:
                    f.seek(offset)
                    return f.read(size)

            raise FuseOSError(5) from None

    def write(self, path, data, offset, fh):
        if self.read_only:
            raise FuseOSError(30)

        buf = self._write_buffer.get(fh, b"")
        if offset == 0:
            buf = data
        elif offset > len(buf):
            buf = buf + b"\x00" * (offset - len(buf)) + data
        else:
            buf = buf[:offset] + data + buf[offset + len(data) :]
        self._write_buffer[fh] = buf
        return len(data)

    def create(self, path, mode):
        if self.read_only:
            raise FuseOSError(30)

        self._fd += 1
        fd = self._fd
        self._open_files[fd] = None
        self._write_buffer[fd] = b""
        return fd

    def flush(self, path, fh):
        if self.read_only or fh not in self._write_buffer:
            return

        data = self._write_buffer.pop(fh, None)
        if data is None:
            return

        filename = Path(path).name
        local_path = self.cache_dir / path.lstrip("/")
        local_path.parent.mkdir(parents=True, exist_ok=True)

        local_path.write_bytes(data)

        try:
            self._run_async(self._upload_local_file(local_path, filename))
        except Exception as e:
            logger.error(f"Upload failed for {path}: {e}")
            raise FuseOSError(5) from None

    async def _upload_local_file(self, local_path: Path, filename: str):
        await self._ensure_connected()
        metadata = await self._vault.upload(local_path)
        self._file_cache[metadata.id] = metadata
        self._path_to_id[f"/{filename}"] = metadata.id
        self._id_to_path[metadata.id] = f"/{filename}"
        self._last_refresh = 0

    def release(self, path, fh):
        self._open_files.pop(fh, None)
        self._write_buffer.pop(fh, None)
        return 0

    def unlink(self, path):
        if self.read_only:
            raise FuseOSError(30)

        file_id = self._path_to_id.get(path)
        if not file_id:
            raise FuseOSError(2)

        try:
            self._run_async(self._ensure_connected())
            self._run_async(self._vault.delete(file_id))
            self._file_cache.pop(file_id, None)
            self._path_to_id.pop(path, None)
            self._id_to_path.pop(file_id, None)
            self._chunk_caches.pop(file_id, None)
            self._last_refresh = 0

            local_path = self.cache_dir / path.lstrip("/")
            if local_path.exists():
                local_path.unlink()
        except Exception as e:
            logger.error(f"Delete failed for {path}: {e}")
            raise FuseOSError(5) from None

    def statfs(self, path):
        return {
            "f_bsize": 4096,
            "f_blocks": 1024 * 1024,
            "f_bavail": 1024 * 1024,
            "f_bfree": 1024 * 1024,
            "f_files": len(self._file_cache),
        }

    def destroy(self, private_data):
        if self._vault:
            self._run_async(self._vault.disconnect())
        if self._loop and not self._loop.is_closed():
            self._loop.close()


def mount_vault(
    mount_point: str,
    config: Config | None = None,
    telegram_config: TelegramConfig | None = None,
    password: str | None = None,
    read_only: bool = False,
    cache_dir: str | None = None,
    foreground: bool = True,
    allow_other: bool = False,
    cache_size_mb: int = 100,
):
    """Mount the TeleVault as a FUSE filesystem with on-demand chunk fetching."""
    if not FUSE_AVAILABLE:
        print(
            "Error: fusepy is required for FUSE mount.\n"
            "Install with: pipx install televault[fuse]\n\n"
            "On Linux, you may also need: sudo apt install fuse libfuse2"
        )
        sys.exit(1)

    fuse_ops = TeleVaultFuse(
        config=config,
        telegram_config=telegram_config,
        password=password,
        cache_dir=cache_dir,
        read_only=read_only,
        cache_size_mb=cache_size_mb,
    )

    print("Connecting to vault and preloading file index...")
    try:
        fuse_ops._run_async(fuse_ops._refresh_index(force=True))
        file_count = len(fuse_ops._file_cache)
        print(f"  Loaded {file_count} file{'s' if file_count != 1 else ''} from vault")
    except Exception as e:
        print(f"  Warning: preload failed ({e}), files will load on first access")

    fuse_opts = {"ro" if read_only else "rw": True}
    if allow_other:
        fuse_opts["allow_other"] = True

    print(f"Mounting TeleVault at {mount_point}")
    print(f"  Cache: {cache_size_mb}MB LRU, on-demand chunk fetching")
    print("  Press Ctrl+C to unmount")

    try:
        FUSE(fuse_ops, mount_point, foreground=foreground, **fuse_opts)
    except KeyboardInterrupt:
        print("\nUnmounting...")
    except Exception as e:
        print(f"FUSE error: {e}")
        print("Make sure FUSE is installed and the mount point exists.")
        print("On Linux: sudo apt install fuse libfuse2")
        print("On macOS: install macFUSE from https://macfuse.io/")
        sys.exit(1)
