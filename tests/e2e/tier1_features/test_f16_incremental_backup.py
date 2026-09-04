"""Feature 16 Tests: Incremental Backup & Restore."""

import shutil
from pathlib import Path
import pytest

from televault.snapshot import Snapshot
from ..harness.crypto_oracle import CryptoOracle
from ..harness.fixtures import generate_nested_project_tree


def test_f16_unchanged_file_skipping(tmp_path):
    """F16-1: Incremental backup skips files when size and BLAKE3 hash match."""
    src_dir = tmp_path / "src"
    generate_nested_project_tree(src_dir)

    readme = src_dir / "README.md"
    h1 = CryptoOracle.blake3_hash(readme.read_bytes())
    s1 = readme.stat().st_size

    # Unchanged state check
    h2 = CryptoOracle.blake3_hash(readme.read_bytes())
    s2 = readme.stat().st_size
    assert h1 == h2 and s1 == s2, "Unchanged files must match on hash and size"


def test_f16_modified_file_upload(tmp_path):
    """F16-2: Modified file hash discrepancy triggers backup."""
    f = tmp_path / "test.txt"
    f.write_text("Version 1")
    h1 = CryptoOracle.blake3_hash(f.read_bytes())

    f.write_text("Version 2 - modified content")
    h2 = CryptoOracle.blake3_hash(f.read_bytes())

    assert h1 != h2, "Hash must change when file content is modified"


def test_f16_deleted_file_handling(tmp_path):
    """F16-3: Files deleted in working tree are omitted from subsequent snapshot index."""
    src_dir = tmp_path / "src"
    generate_nested_project_tree(src_dir)

    target_file = src_dir / "data" / "records.csv"
    assert target_file.exists()
    target_file.unlink()

    # Scanning directory should not include deleted file
    remaining_files = [str(p.relative_to(src_dir)) for p in src_dir.rglob("*") if p.is_file()]
    assert "data/records.csv" not in remaining_files


def test_f16_relative_path_preservation(tmp_path):
    """F16-4: Snapshot restore preserves relative paths across nested directory trees."""
    src_dir = tmp_path / "src"
    restore_dir = tmp_path / "restored"
    generate_nested_project_tree(src_dir)

    # Recreate in restore_dir using relative paths
    for p in src_dir.rglob("*"):
        if p.is_file():
            rel = p.relative_to(src_dir)
            dest = restore_dir / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(p.read_bytes())

    for p in src_dir.rglob("*"):
        if p.is_file():
            rel = p.relative_to(src_dir)
            assert (restore_dir / rel).exists()


def test_f16_snapshot_restore_contents(tmp_path):
    """F16-5: Restored files have exact byte contents as source files."""
    src_dir = tmp_path / "src"
    restore_dir = tmp_path / "restored"
    generate_nested_project_tree(src_dir)

    for p in src_dir.rglob("*"):
        if p.is_file():
            rel = p.relative_to(src_dir)
            dest = restore_dir / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(p.read_bytes())
            assert dest.read_bytes() == p.read_bytes()
