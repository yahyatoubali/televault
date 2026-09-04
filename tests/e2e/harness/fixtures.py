"""Test fixtures, temporary environments, and data generators."""

import json
import os
from pathlib import Path
import shutil
import tempfile
from typing import Dict, Generator, List, Optional


class IsolatedEnvironment:
    """Provides a completely isolated XDG configuration and data environment."""

    def __init__(self, prefix: str = "tv_e2e_"):
        self.root_dir = Path(tempfile.mkdtemp(prefix=prefix))
        self.config_dir = self.root_dir / "config"
        self.data_dir = self.root_dir / "data"
        self.workspace_dir = self.root_dir / "workspace"

        self.config_dir.mkdir(parents=True, exist_ok=True)
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.workspace_dir.mkdir(parents=True, exist_ok=True)

    def write_config(
        self,
        channel_id: int = -1001234567890,
        chunk_size: int = 104857600,
        encryption: bool = True,
        compression: bool = True,
        low_resource: bool = False,
        api_id: int = 123456,
        api_hash: str = "mock_api_hash_abcdef0123456789",
    ) -> Path:
        """Writes a valid televault config.json into config_dir."""
        tv_dir = self.config_dir / "televault"
        tv_dir.mkdir(parents=True, exist_ok=True)
        config_path = tv_dir / "config.json"

        data = {
            "channel_id": channel_id,
            "chunk_size": chunk_size,
            "encryption": encryption,
            "compression": compression,
            "index_msg_id": 999,
            "low_resource": {
                "chunk_size": 33554432,
                "enabled": low_resource,
                "hasher_threads": 1,
                "max_no_compress_size": 524288000,
                "parallel_downloads": 2,
                "parallel_uploads": 2,
                "sequential_download": True,
            },
            # Also include backward-compatible flat fields
            "low_resource_mode": low_resource,
            "parallel_downloads": 3,
            "parallel_uploads": 3,
            "retry": {
                "base_delay_ms": 100,
                "jitter_factor": 0.1,
                "max_delay_ms": 500,
                "max_retries": 3,
            },
            "telegram": {
                "api_id": api_id,
                "api_hash": api_hash,
                "phone": "+1234567890",
            },
        }

        with open(config_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

        return config_path

    def cleanup(self):
        """Removes temporary environment directories."""
        if self.root_dir.exists():
            shutil.rmtree(self.root_dir, ignore_errors=True)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.cleanup()


def generate_test_file(path: Path, size_bytes: int, pattern: Optional[bytes] = None) -> Path:
    """Creates a deterministic file of specific size."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "wb") as f:
        if size_bytes == 0:
            return path

        if pattern:
            repeats = size_bytes // len(pattern)
            remainder = size_bytes % len(pattern)
            for _ in range(repeats):
                f.write(pattern)
            if remainder:
                f.write(pattern[:remainder])
        else:
            # Pseudo-random but deterministic content
            block = bytearray(min(size_bytes, 65536))
            remaining = size_bytes
            seed = 42
            while remaining > 0:
                chunk = min(remaining, len(block))
                for i in range(chunk):
                    seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
                    block[i] = seed & 0xFF
                f.write(memoryview(block)[:chunk])
                remaining -= chunk
    return path


def generate_nested_project_tree(root_dir: Path) -> List[Path]:
    """Generates a realistic multi-level project tree with code, docs, and assets."""
    created = []
    files_to_create = {
        "README.md": b"# Sample Project\nThis is an end-to-end workload test repository.\n",
        "src/main.cpp": b"#include <iostream>\nint main() { std::cout << \"Hello E2E\"; return 0; }\n",
        "src/utils/helper.hpp": b"#pragma once\ninline int add(int a, int b) { return a + b; }\n",
        "assets/images/logo.png": b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR" + b"\x00" * 32,
        "assets/docs/manual.pdf": b"%PDF-1.4\n1 0 obj\n<< /Title (Test) >>\nendobj\n",
        "config/app_settings.json": b'{"env": "production", "workers": 4, "timeout": 30}\n',
        "data/records.csv": b"id,name,role\n1,alice,admin\n2,bob,developer\n3,carol,analyst\n",
        ".hidden_config": b"SECRET_KEY=123456\n",
    }
    for rel_path, content in files_to_create.items():
        full_path = root_dir / rel_path
        full_path.parent.mkdir(parents=True, exist_ok=True)
        with open(full_path, "wb") as f:
            f.write(content)
        created.append(full_path)
    return created
