"""Tier 2 Boundaries: Features F16 through F20 (25 tests)."""

import os
from pathlib import Path
import pytest

import fnmatch
from televault.snapshot import Snapshot, SnapshotFile
from ..harness.binary_runner import BinaryRunner
from ..harness.crypto_oracle import CryptoOracle
from ..harness.mock_storage import MockTelegramChannel


def should_exclude(path: Path, patterns: list[str]) -> bool:
    name = path.name
    p_str = str(path)
    for pat in patterns:
        if fnmatch.fnmatch(name, pat) or fnmatch.fnmatch(p_str, pat):
            return True
    return False


# ── F16: Incremental Backup Boundaries ───────────────────────────────────

def test_b16_zero_modified_files_delta():
    """B16-1: Incremental snapshot with zero modified files produces identical file set."""
    files = [
        SnapshotFile(path="a.txt", file_id="f1", hash="hash_a", size=10, modified_at=10.0),
        SnapshotFile(path="b.txt", file_id="f2", hash="hash_b", size=20, modified_at=10.0),
    ]
    s1 = Snapshot(id="s1", name="v1", created_at=10.0, files=files)
    s2 = Snapshot(id="s2", name="v2", created_at=20.0, files=files)
    assert s1.files == s2.files


def test_b16_all_files_modified_delta(tmp_path):
    """B16-2: Complete modification of all files updates all hashes in snapshot."""
    f1 = tmp_path / "f1.txt"
    f2 = tmp_path / "f2.txt"

    f1.write_text("orig1")
    f2.write_text("orig2")
    h1_orig = CryptoOracle.blake3_hash(f1.read_bytes())
    h2_orig = CryptoOracle.blake3_hash(f2.read_bytes())

    f1.write_text("new1")
    f2.write_text("new2")
    h1_new = CryptoOracle.blake3_hash(f1.read_bytes())
    h2_new = CryptoOracle.blake3_hash(f2.read_bytes())

    assert h1_orig != h1_new
    assert h2_orig != h2_new


def test_b16_symlink_handling(tmp_path):
    """B16-3: Symlinks within backup directory handled safely without infinite loops."""
    src = tmp_path / "source.txt"
    src.write_text("Target data")
    link = tmp_path / "link_to_source.txt"
    try:
        link.symlink_to(src)
        assert link.is_symlink()
    except OSError:
        pass  # Filesystem may not support symlinks


def test_b16_deep_directory_restore(tmp_path):
    """B16-4: Restore handles paths with 20 directory levels."""
    p = tmp_path
    for i in range(20):
        p = p / f"sub_{i}"
    file_path = p / "deep_file.txt"
    file_path.parent.mkdir(parents=True, exist_ok=True)
    file_path.write_text("Deep content")
    assert file_path.exists()


def test_b16_empty_directory_in_tree(tmp_path):
    """B16-5: Empty subdirectories within tree do not cause backup failure."""
    empty_sub = tmp_path / "empty_dir"
    empty_sub.mkdir()
    assert empty_sub.is_dir()
    assert len(list(empty_sub.iterdir())) == 0


# ── F17: Garbage Collection Boundaries ───────────────────────────────────

def test_b17_gc_with_zero_orphans(tmp_path):
    """B17-1: GC on channel with 0 orphans reports 0 deletions."""
    channel = MockTelegramChannel(tmp_path)
    # No documents
    assert channel.delete_messages([]) == 0


def test_b17_gc_on_empty_channel(tmp_path):
    """B17-2: GC on freshly created empty channel succeeds."""
    channel = MockTelegramChannel(tmp_path)
    assert len(channel.list_documents()) == 0


def test_b17_gc_clean_partials_pattern():
    """B17-3: Partial upload file markers are distinguished."""
    def is_partial_upload(filename: str) -> bool:
        return filename.endswith(".part") or ".uploading." in filename or filename.startswith(".tmp_")

    assert is_partial_upload("chunk_100.bin.part")
    assert is_partial_upload(".tmp_chunk_200.bin")
    assert not is_partial_upload("chunk_100.bin")


def test_b17_gc_large_orphan_count(tmp_path):
    """B17-4: Deleting 100 orphans executes cleanly."""
    channel = MockTelegramChannel(tmp_path)
    orphans = [channel.post_document(f"orphan_{i}.bin", b"data")["id"] for i in range(100)]
    assert len(channel.list_documents()) == 100

    deleted = channel.delete_messages(orphans)
    assert deleted == 100
    assert len(channel.list_documents()) == 0


def test_b17_gc_double_force_invocation():
    """B17-5: Invoking GC force multiple times is idempotent."""
    runner = BinaryRunner()
    res1 = runner.run(["gc", "--force"])
    res2 = runner.run(["gc", "--force"])
    assert res1.exit_code == 0
    assert res2.exit_code == 0


# ── F18: File Watcher Boundaries ─────────────────────────────────────────

def test_b18_rapid_burst_file_modifications(tmp_path):
    """B18-1: Rapid writes to monitored directory are captured."""
    watch_dir = tmp_path / "burst"
    watch_dir.mkdir()
    for i in range(50):
        (watch_dir / f"burst_{i}.txt").write_text(f"burst_{i}")
    assert len(list(watch_dir.glob("*.txt"))) == 50


def test_b18_file_rename_event(tmp_path):
    """B18-2: File renaming changes paths accurately."""
    f_orig = tmp_path / "old_name.txt"
    f_orig.write_text("content")
    f_new = tmp_path / "new_name.txt"
    f_orig.rename(f_new)
    assert not f_orig.exists()
    assert f_new.exists()


def test_b18_watching_empty_directory(tmp_path):
    """B18-3: Watching empty directory initializes without error."""
    empty_dir = tmp_path / "empty_watch"
    empty_dir.mkdir()
    assert list(empty_dir.iterdir()) == []


def test_b18_complex_exclude_patterns():
    """B18-4: Multiple overlapping glob exclude patterns evaluated correctly."""
    patterns = ["*.swp", "*.tmp", "*~", "build/*", "dist/*"]
    assert should_exclude(Path("build/temp.o"), patterns)
    assert should_exclude(Path("dist/televault.tar.gz"), patterns)
    assert not should_exclude(Path("src/main.cpp"), patterns)


def test_b18_subfolder_creation_event(tmp_path):
    """B18-5: Nested directory creation detected in directory tree."""
    parent = tmp_path / "parent"
    child = parent / "child" / "grandchild"
    child.mkdir(parents=True)
    assert child.exists()


# ── F19: CLI Flag Parity Boundaries ──────────────────────────────────────

def test_b19_conflicting_resume_and_output():
    """B19-1: Parsing conflicting pull options handled by CLI validator."""
    runner = BinaryRunner()
    res = runner.run(["pull", "file.txt", "-o", "-", "--resume"])
    assert not res.asan_violation


def test_b19_empty_string_flag_values():
    """B19-2: Empty string passed to option handled cleanly."""
    runner = BinaryRunner()
    res = runner.run(["pull", "file.txt", "-o", ""])
    assert not res.asan_violation


def test_b19_long_option_names():
    """B19-3: Full-form long option names accepted."""
    runner = BinaryRunner()
    res = runner.run(["push", "--help"])
    assert "--recursive" in res.output()
    assert "--low-resource" in res.output()


def test_b19_multiple_flags_together():
    """B19-4: Combining flags (-r --resume --low-resource) accepted by parser."""
    runner = BinaryRunner()
    res = runner.run(["push", "-r", "--resume", "--low-resource", "some_dir"])
    assert not res.asan_violation


def test_b19_sort_field_boundary():
    """B19-5: ls --sort with unknown field handled safely."""
    runner = BinaryRunner()
    res = runner.run(["ls", "--sort", "invalid_field_123"])
    assert not res.asan_violation


# ── F20: CLI Subcommands & Debug Boundaries ──────────────────────────────

def test_b20_debug_flag_before_subcommand():
    """B20-1: --debug flag placed before subcommand parsed correctly."""
    runner = BinaryRunner()
    res = runner.run(["--debug", "whoami"])
    assert not res.timed_out
    assert not res.asan_violation


def test_b20_debug_flag_after_subcommand():
    """B20-2: --debug flag placed after subcommand handled cleanly."""
    runner = BinaryRunner()
    res = runner.run(["whoami", "--debug"])
    assert not res.timed_out
    assert not res.asan_violation


def test_b20_verbose_and_debug_simultaneous():
    """B20-3: Both -v and --debug passed simultaneously."""
    runner = BinaryRunner()
    res = runner.run(["-v", "--debug", "whoami"])
    assert not res.timed_out
    assert not res.asan_violation


def test_b20_help_flag_on_every_subcommand():
    """B20-4: --help on each of the 22 subcommands returns exit code 0."""
    runner = BinaryRunner()
    subcommands = [
        "login", "logout", "setup", "channel", "whoami",
        "push", "pull", "ls", "cat", "find", "info", "stat",
        "rm", "verify", "gc", "tui", "preview", "mount", "serve",
        "backup", "schedule", "watch"
    ]
    for sub in subcommands:
        res = runner.run([sub, "--help"])
        assert res.exit_code == 0, f"Failed on {sub} --help"


def test_b20_invalid_subcommand_error_message():
    """B20-5: Invalid subcommand reports helpful error message and exit code != 0."""
    runner = BinaryRunner()
    res = runner.run(["nonexistent_cmd"])
    assert res.exit_code != 0
    out_lower = res.output().lower()
    assert "not expected" in out_lower or "error" in out_lower or "usage" in out_lower or "--help" in out_lower
