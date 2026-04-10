"""FUSE filesystem driver for TeleVault - mount your vault as a local directory."""

import asyncio
import logging
import os
import stat
import sys
import time
from pathlib import Path

from .config import Config, get_data_dir
from .core import TeleVault
from .models import FileMetadata
from .telegram import TelegramConfig

logger = logging.getLogger("televault.fuse")

FUSE_STATVFS = None

try:
    from fuse import FUSE, FuseOSError, Operations as FuseOperations

    FUSE_AVAILABLE = True
except ImportError:
    FUSE_AVAILABLE = False


class TeleVaultFuse(FuseOperations if FUSE_AVAILABLE else object):
    """FUSE filesystem that maps TeleVault storage to a local mount point."""

    def __init__(
        self,
        config: Config | None = None,
        telegram_config: TelegramConfig | None = None,
        password: str | None = None,
        cache_dir: str | None = None,
        read_only: bool = False,
    ):
        if not FUSE_AVAILABLE:
            raise ImportError(
                "fusepy is required for FUSE mount. Install with: pip install televault[fuse]"
            )

        self.config = config or Config.load_or_create()
        self.password = password
        self.read_only = read_only
        self._vault: TeleVault | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._telegram_config = telegram_config

        cache_path = Path(cache_dir) if cache_dir else get_data_dir() / "fuse_cache"
        self.cache_dir = cache_path
        self.cache_dir.mkdir(parents=True, exist_ok=True)

        self._file_cache: dict[str, FileMetadata] = {}
        self._path_to_id: dict[str, str] = {}
        self._id_to_path: dict[str, str] = {}
        self._fd = 0
        self._open_files: dict[int, Path] = {}
        self._write_buffer: dict[int, bytes] = {}
        self._cache_lock = asyncio.Lock()
        self._refresh_task: asyncio.Task | None = None
        self._last_refresh = 0.0

    def _run_async(self, coro):
        if self._loop is None:
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)

        try:
            running_loop = asyncio.get_running_loop()
        except RuntimeError:
            running_loop = None

        if running_loop and running_loop.is_running():
            import concurrent.futures

            with concurrent.futures.ThreadPoolExecutor() as executor:
                future = executor.submit(self._loop.run_until_complete, coro)
                return future.result()
        else:
            return self._loop.run_until_complete(coro)

    async def _ensure_connected(self):
        if self._vault is None:
            self._vault = TeleVault(
                config=self.config,
                telegram_config=self._telegram_config,
                password=self.password,
            )
            await self._vault.connect()

    async def _refresh_index(self):
        await self._ensure_connected()
        now = time.time()
        if now - self._last_refresh < 2.0:
            return

        files = await self._vault.list_files()
        async with self._cache_lock:
            self._file_cache.clear()
            self._path_to_id.clear()
            self._id_to_path.clear()
            for f in files:
                self._file_cache[f.id] = f
                self._path_to_id[f"/{f.name}"] = f.id
                self._id_to_path[f.id] = f"/{f.name}"
            self._last_refresh = now

    def _get_stat(self, is_dir=False, size=0):
        now = time.time()
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

        self._run_async(self._refresh_index())

        if path in self._path_to_id:
            file_id = self._path_to_id[path]
            if file_id in self._file_cache:
                meta = self._file_cache[file_id]
                return self._get_stat(is_dir=False, size=meta.size)

        cached_file = self.cache_dir / path.lstrip("/")
        if cached_file.exists():
            return self._get_stat(is_dir=False, size=cached_file.stat().st_size)

        raise FuseOSError(2)

    def readdir(self, path, fh):
        entries = [".", ".."]

        self._run_async(self._refresh_index())

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
        local_path = self.cache_dir / path.lstrip("/")

        if not local_path.exists():
            local_path.parent.mkdir(parents=True, exist_ok=True)
            try:
                self._run_async(self._vault.download(file_id, output_path=str(local_path)))
            except Exception as e:
                logger.error(f"Download failed for {path}: {e}")
                raise FuseOSError(5)

        self._open_files[fd] = local_path
        return fd

    def read(self, path, size, offset, fh):
        local_path = self._open_files.get(fh)
        if local_path is None:
            local_path = self.cache_dir / path.lstrip("/")
        if not local_path.exists():
            raise FuseOSError(2)

        with open(local_path, "rb") as f:
            f.seek(offset)
            return f.read(size)

    def write(self, path, data, offset, fh):
        if self.read_only:
            raise FuseOSError(30)

        buf = self._write_buffer.get(fh, b"")
        if offset == 0:
            buf = data
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
            raise FuseOSError(5)

    async def _upload_local_file(self, local_path: Path, filename: str):
        await self._ensure_connected()
        metadata = await self._vault.upload(local_path)
        self._file_cache[metadata.id] = metadata
        self._path_to_id[f"/{filename}"] = metadata.id
        self._id_to_path[metadata.id] = f"/{filename}"

    def unlink(self, path):
        if self.read_only:
            raise FuseOSError(30)

        file_id = self._path_to_id.get(path)
        if not file_id:
            raise FuseOSError(2)

        try:
            self._run_async(self._vault.delete(file_id))
            self._file_cache.pop(file_id, None)
            self._path_to_id.pop(path, None)
            self._id_to_path.pop(file_id, None)
        except Exception as e:
            logger.error(f"Delete failed for {path}: {e}")
            raise FuseOSError(5)

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
):
    """Mount the TeleVault as a FUSE filesystem."""
    if not FUSE_AVAILABLE:
        print(
            "Error: fusepy is required for FUSE mount.\n"
            "Install with: pip install televault[fuse]\n\n"
            "On Linux, you may also need: sudo apt install fuse libfuse2"
        )
        sys.exit(1)

    fuse_ops = TeleVaultFuse(
        config=config,
        telegram_config=telegram_config,
        password=password,
        cache_dir=cache_dir,
        read_only=read_only,
    )

    fuse_opts = {}
    if allow_other:
        fuse_opts["allow_other"] = True

    print(f"Mounting TeleVault at {mount_point}")
    print("Press Ctrl+C to unmount")

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
