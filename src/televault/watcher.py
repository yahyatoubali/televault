"""Filesystem watcher for TeleVault - automatically backup changed files."""

import asyncio
import hashlib
import logging
import os
import time
from pathlib import Path

from .config import Config, get_data_dir
from .core import TeleVault
from .telegram import TelegramConfig

logger = logging.getLogger("televault.watcher")

WATCHER_STATE_FILE = "watcher_state.json"


class FileWatcher:
    """Watch directories for changes and automatically upload to TeleVault."""

    def __init__(
        self,
        config: Config | None = None,
        telegram_config: TelegramConfig | None = None,
        password: str | None = None,
        debounce_seconds: float = 2.0,
        exclude_patterns: list[str] | None = None,
    ):
        self.config = config or Config.load_or_create()
        self.password = password
        self._telegram_config = telegram_config
        self.debounce_seconds = debounce_seconds
        self._vault: TeleVault | None = None
        self._running = False
        self._watched_dirs: dict[str, dict] = {}
        self._file_hashes: dict[str, str] = {}
        self._pending_uploads: dict[str, float] = {}
        self._exclude_patterns = exclude_patterns or [
            ".git",
            "__pycache__",
            ".DS_Store",
            "Thumbs.db",
            "*.pyc",
            "*.partial",
            "*.tmp",
            ".televault",
        ]
        self._state_dir = get_data_dir() / "watcher"
        self._state_dir.mkdir(parents=True, exist_ok=True)

    def _should_exclude(self, path: str) -> bool:
        import fnmatch

        name = Path(path).name
        for pattern in self._exclude_patterns:
            if fnmatch.fnmatch(name, pattern) or name == pattern:
                return True
        return False

    def _hash_file(self, path: Path) -> str | None:
        try:
            h = hashlib.blake2b(digest_size=16)
            with open(path, "rb") as f:
                while chunk := f.read(65536):
                    h.update(chunk)
            return h.hexdigest()
        except (OSError, PermissionError):
            return None

    async def _ensure_connected(self):
        if self._vault is None:
            self._vault = TeleVault(
                config=self.config,
                telegram_config=self._telegram_config,
                password=self.password,
            )
            await self._vault.connect()

    def add_watch(self, directory: str, recursive: bool = True) -> None:
        """Add a directory to watch."""
        path = Path(directory).resolve()
        if not path.is_dir():
            raise ValueError(f"Not a directory: {path}")

        self._watched_dirs[str(path)] = {
            "recursive": recursive,
            "added_at": time.time(),
        }
        logger.info(f"Watching: {path} (recursive={recursive})")

        self._scan_directory(str(path))

    def remove_watch(self, directory: str) -> bool:
        """Remove a directory from watching."""
        path = str(Path(directory).resolve())
        if path in self._watched_dirs:
            del self._watched_dirs[path]
            to_remove = [k for k in self._file_hashes if k.startswith(path + "/")]
            for k in to_remove:
                del self._file_hashes[k]
            logger.info(f"Stopped watching: {path}")
            return True
        return False

    def _scan_directory(self, directory: str) -> list[str]:
        """Scan a directory and detect changed files."""
        changed = []
        info = self._watched_dirs.get(directory)
        if not info:
            return changed

        recursive = info.get("recursive", True)

        if recursive:
            walker = os.walk(directory)
        else:
            try:
                entries = os.listdir(directory)
                walker = [
                    (
                        directory,
                        [e for e in entries if (Path(directory) / e).is_dir()],
                        [e for e in entries if (Path(directory) / e).is_file()],
                    )
                ]
            except PermissionError:
                return changed

        for dirpath, dirnames, filenames in walker:
            dirnames[:] = [d for d in dirnames if not self._should_exclude(d)]

            for filename in filenames:
                if self._should_exclude(filename):
                    continue

                filepath = os.path.join(dirpath, filename)

                if not Path(filepath).is_file():
                    continue

                if filepath in self._pending_uploads:
                    continue

                new_hash = self._hash_file(Path(filepath))
                if new_hash is None:
                    continue

                old_hash = self._file_hashes.get(filepath)

                if old_hash is None:
                    self._file_hashes[filepath] = new_hash
                    changed.append(filepath)
                    logger.debug(f"New file detected: {filepath}")
                elif new_hash != old_hash:
                    self._file_hashes[filepath] = new_hash
                    changed.append(filepath)
                    logger.debug(f"Changed file detected: {filepath}")

        return changed

    async def _upload_file(self, filepath: str) -> bool:
        """Upload a single file to TeleVault."""
        try:
            await self._ensure_connected()
            path = Path(filepath)

            if not path.exists():
                logger.debug(f"File gone before upload: {filepath}")
                return False

            logger.info(f"Uploading: {filepath}")
            metadata = await self._vault.upload(filepath, password=self.password)
            logger.info(f"Uploaded: {filepath} -> {metadata.id} ({metadata.size} bytes)")
            return True
        except Exception as e:
            logger.error(f"Upload failed for {filepath}: {e}")
            return False

    async def _process_changes(self):
        """Scan all watched directories and upload changes."""
        changed_files = []
        for directory in list(self._watched_dirs.keys()):
            if Path(directory).exists():
                changed_files.extend(self._scan_directory(directory))

        for filepath in changed_files:
            await self._upload_file(filepath)

    async def watch(self, poll_interval: float = 5.0):
        """Start watching for file changes. Runs until stopped."""
        self._running = True
        logger.info(f"File watcher started (poll interval: {poll_interval}s)")

        all_changed = []
        for directory in list(self._watched_dirs.keys()):
            if Path(directory).exists():
                all_changed.extend(self._scan_directory(directory))

        for filepath in all_changed:
            logger.info(f"Initial scan - uploading: {filepath}")
            await self._upload_file(filepath)

        logger.info(f"Initial scan complete. Watching {len(self._file_hashes)} files for changes.")

        try:
            while self._running:
                await asyncio.sleep(poll_interval)
                await self._process_changes()
        except asyncio.CancelledError:
            logger.info("File watcher stopped")
        finally:
            self._running = False

    def stop(self):
        """Stop the file watcher."""
        self._running = False

    async def status(self) -> dict:
        """Get watcher status."""
        return {
            "watched_dirs": list(self._watched_dirs.keys()),
            "tracked_files": len(self._file_hashes),
            "running": self._running,
        }

    def save_state(self):
        """Save watcher state to disk."""
        state_file = self._state_dir / WATCHER_STATE_FILE
        import json

        state = {
            "file_hashes": self._file_hashes,
            "watched_dirs": self._watched_dirs,
            "saved_at": time.time(),
        }
        state_file.write_text(json.dumps(state, indent=2))

    def load_state(self) -> bool:
        """Load watcher state from disk."""
        import json

        state_file = self._state_dir / WATCHER_STATE_FILE
        if not state_file.exists():
            return False

        try:
            state = json.loads(state_file.read_text())
            self._file_hashes = state.get("file_hashes", {})
            self._watched_dirs = state.get("watched_dirs", {})
            logger.info(f"Loaded watcher state: {len(self._file_hashes)} tracked files")
            return True
        except Exception as e:
            logger.warning(f"Failed to load watcher state: {e}")
            return False
