"""Configuration management for TeleVault."""

import contextlib
import json
import os
import tempfile
from dataclasses import asdict, dataclass
from pathlib import Path


def get_config_dir() -> Path:
    """Get TeleVault config directory."""
    if os.name == "nt":  # Windows
        base = Path(os.environ.get("APPDATA", "~"))
    else:  # Unix
        base = Path(os.environ.get("XDG_CONFIG_HOME", "~/.config"))

    config_dir = base.expanduser() / "televault"
    config_dir.mkdir(parents=True, exist_ok=True)
    return config_dir


def get_data_dir() -> Path:
    """Get TeleVault data directory (for temp files, cache)."""
    if os.name == "nt":
        base = Path(os.environ.get("LOCALAPPDATA", "~"))
    else:
        base = Path(os.environ.get("XDG_DATA_HOME", "~/.local/share"))

    data_dir = base.expanduser() / "televault"
    data_dir.mkdir(parents=True, exist_ok=True)
    return data_dir


@dataclass
class Config:
    """TeleVault configuration."""

    # Telegram settings
    channel_id: int | None = None

    # Cached message IDs for O(1) index lookups
    index_msg_id: int | None = None
    snapshot_index_msg_id: int | None = None

    # Chunking
    chunk_size: int = 256 * 1024 * 1024  # 256MB (better for fast connections)

    # Processing options
    compression: bool = True
    encryption: bool = True

    # Concurrency (tuned for 100+ Mbps connections)
    parallel_uploads: int = 8
    parallel_downloads: int = 10

    # Retry settings
    max_retries: int = 3
    retry_delay: float = 1.0

    def save(self) -> None:
        """Save config to file atomically (write temp + rename)."""
        config_path = get_config_dir() / "config.json"
        config_path.parent.mkdir(parents=True, exist_ok=True)
        data = json.dumps(asdict(self), indent=2)
        tmp_fd, tmp_path = tempfile.mkstemp(dir=config_path.parent, suffix=".tmp")
        try:
            with os.fdopen(tmp_fd, "w") as f:
                f.write(data)
                f.flush()
                os.fsync(f.fileno())
            os.replace(tmp_path, config_path)
        except Exception:
            with contextlib.suppress(OSError):
                os.unlink(tmp_path)
            raise

    @classmethod
    def load(cls) -> "Config":
        """Load config from file."""
        config_path = get_config_dir() / "config.json"

        if not config_path.exists():
            return cls()

        with open(config_path) as f:
            data = json.load(f)

        return cls(**data)

    @classmethod
    def load_or_create(cls) -> "Config":
        """Load config or create default."""
        config = cls.load()
        if not (get_config_dir() / "config.json").exists():
            config.save()
        return config
