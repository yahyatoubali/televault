"""Configuration management for TeleVault."""

import json
from pathlib import Path
from dataclasses import dataclass, field, asdict
from typing import Optional
import os


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
    channel_id: Optional[int] = None
    
    # Chunking
    chunk_size: int = 100 * 1024 * 1024  # 100MB
    
    # Processing options
    compression: bool = True
    encryption: bool = True
    
    # Concurrency
    parallel_uploads: int = 3
    parallel_downloads: int = 5
    
    # Retry settings
    max_retries: int = 3
    retry_delay: float = 1.0
    
    def save(self) -> None:
        """Save config to file."""
        config_path = get_config_dir() / "config.json"
        with open(config_path, "w") as f:
            json.dump(asdict(self), f, indent=2)
    
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
