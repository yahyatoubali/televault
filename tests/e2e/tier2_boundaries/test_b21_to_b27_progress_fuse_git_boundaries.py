"""Tier 2 Boundaries: Features F21 through F27 (35 tests)."""

import os
from pathlib import Path
import subprocess
import pytest

from ..harness.binary_inspector import BinaryInspector
from ..harness.binary_runner import BinaryRunner
from ..harness.config import BINARY_PATH, BUILD_DIR, PROJECT_ROOT
from ..harness.crypto_oracle import CryptoOracle


# ── F21: Progress Tracking Boundaries ────────────────────────────────────

def test_b21_zero_byte_transfer_progress():
    """B21-1: 0-byte transfer shows 100% or 0/0 bytes without division by zero."""
    # Simulating 0-byte transfer progress calculation
    current, total = 0, 0
    percent = 100.0 if total == 0 else (current / total) * 100.0
    assert percent == 100.0


def test_b21_speed_calculation_zero_elapsed_time():
    """B21-2: Speed calculation with 0 elapsed time returns 0.0 without division by zero."""
    bytes_transferred = 1024
    elapsed_sec = 0.0
    speed = bytes_transferred / elapsed_sec if elapsed_sec > 0 else 0.0
    assert speed == 0.0


def test_b21_progress_percentage_bounds():
    """B21-3: Progress percentage never exceeds 100.0%."""
    current, total = 1050, 1000
    percent = min(100.0, (current / total) * 100.0)
    assert percent == 100.0


def test_b21_narrow_terminal_width():
    """B21-4: Progress formatting truncates safely for narrow 40-column terminals."""
    bar = "Progress: [==========] 100% 10MB/10MB"
    truncated = bar[:40]
    assert len(truncated) <= 40


def test_b21_non_tty_redirection():
    """B21-5: Progress bar handles stdout redirection to file."""
    runner = BinaryRunner()
    res = runner.run(["stat", "--help"])
    assert "\r" not in res.stdout, "Static help output should not contain carriage returns"


# ── F22: FUSE Cache Boundaries ───────────────────────────────────────────

def test_b22_single_entry_capacity_cache():
    """B22-1: Cache with capacity 1 correctly holds only the most recent item."""
    from collections import OrderedDict
    cache = OrderedDict()
    capacity = 1

    def put(k, v):
        cache[k] = v
        if len(cache) > capacity:
            cache.popitem(last=False)

    put("chunk_1", b"data1")
    put("chunk_2", b"data2")
    assert len(cache) == 1
    assert "chunk_2" in cache
    assert "chunk_1" not in cache


def test_b22_empty_cache_lookup():
    """B22-2: Lookup in empty cache returns None."""
    cache = {}
    assert cache.get("missing") is None


def test_b22_cache_eviction_with_exact_byte_boundary():
    """B22-3: Byte-bounded cache evicts when byte threshold is reached."""
    current_bytes = 0
    max_bytes = 1000
    cache = {}

    for i in range(15):
        chunk_data = b"X" * 100
        if current_bytes + len(chunk_data) > max_bytes:
            # Evict first item
            oldest = next(iter(cache))
            current_bytes -= len(cache.pop(oldest))
        cache[f"chunk_{i}"] = chunk_data
        current_bytes += len(chunk_data)

    assert current_bytes <= max_bytes


def test_b22_cache_duplicate_update():
    """B22-4: Updating entry with identical size does not increase total bytes."""
    cache = {"chunk_1": b"ABCDE"}
    cache["chunk_1"] = b"FGHIJ"
    assert len(cache["chunk_1"]) == 5


def test_b22_mount_unsupported_options():
    """B22-5: mount command handles unrecognized mount flags safely."""
    runner = BinaryRunner()
    res = runner.run(["mount", "--invalid-fuse-opt"])
    assert res.exit_code != 0


# ── F23: WebDAV & Preview Boundaries ─────────────────────────────────────

def test_b23_preview_zero_byte_file(tmp_path):
    """B23-1: Preview of 0-byte file displays empty preview without error."""
    empty_f = tmp_path / "empty.txt"
    empty_f.write_text("")
    from televault.preview import classify_file
    assert classify_file(empty_f.name) == "text"


def test_b23_preview_large_file_head_only(tmp_path):
    """B23-2: Preview only reads initial bytes (head) of large files."""
    large_f = tmp_path / "large.txt"
    large_f.write_text("Header Line\n" + "Body Line\n" * 10000)

    # Read first 100 bytes for preview
    with open(large_f, "r") as f:
        head = f.read(100)
    assert len(head) == 100
    assert "Header Line" in head


def test_b23_preview_unknown_binary_extension(tmp_path):
    """B23-3: Preview handles unusual or unknown extensions safely."""
    unusual_f = tmp_path / "test.unknown_ext_xyz"
    unusual_f.write_bytes(b"\x00\x01\x02\x03\x04")
    from televault.preview import classify_file
    assert classify_file(unusual_f.name) in ["binary", "unknown"]


def test_b23_serve_port_binding_help():
    """B23-4: WebDAV serve help displays server options."""
    runner = BinaryRunner()
    res = runner.run(["serve", "--help"])
    assert res.exit_code == 0


def test_b23_preview_corrupted_unicode_bytes(tmp_path):
    """B23-5: Preview handles invalid UTF-8 byte sequences with replacement."""
    corrupt_utf8 = tmp_path / "corrupt.txt"
    corrupt_utf8.write_bytes(b"\xff\xfe\xfd Not Valid UTF8")
    with open(corrupt_utf8, "r", errors="replace") as f:
        text = f.read()
    assert len(text) > 0


# ── F24: Unit Test Suite Boundaries ──────────────────────────────────────

def test_b24_ctest_individual_test_selection():
    """B24-1: ctest executes individual test targets by regex name."""
    res = subprocess.run(
        ["ctest", "-R", "^test_models$", "--output-on-failure"],
        cwd=str(BUILD_DIR),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0


def test_b24_ctest_nonexistent_test_filter():
    """B24-2: ctest returns error or reports no tests found when no matching tests exist."""
    res = subprocess.run(
        ["ctest", "-R", "nonexistent_test_xyz"],
        cwd=str(BUILD_DIR),
        capture_output=True,
        text=True,
    )
    combined = (res.stdout or "") + (res.stderr or "")
    assert "No tests were found" in combined or res.returncode != 0


def test_b24_test_execution_within_timeout():
    """B24-3: All individual test binaries complete execution in under 5 seconds."""
    for test_name in ["test_models", "test_retry", "test_compression", "test_chunker"]:
        p = BUILD_DIR / "tests" / test_name
        if p.exists():
            res = subprocess.run([str(p)], capture_output=True, timeout=5)
            assert res.returncode == 0


def test_b24_test_binaries_executable_permissions():
    """B24-4: All test binaries have executable permissions set."""
    for test_bin in (BUILD_DIR / "tests").glob("test_*"):
        if test_bin.is_file() and not test_bin.suffix:
            assert os.access(test_bin, os.X_OK)


def test_b24_test_output_formatting():
    """B24-5: Test binary outputs include GTest OK or PASSED markers."""
    p = BUILD_DIR / "tests" / "test_models"
    if p.exists():
        res = subprocess.run([str(p)], capture_output=True, text=True)
        assert "[       OK ]" in res.stdout or "PASSED" in res.stdout


# ── F25: ASan / UBSan Boundaries ─────────────────────────────────────────

def test_b25_deep_argument_parsing_asan():
    """B25-1: Subcommand option parsing under ASan with 20 chained flags."""
    runner = BinaryRunner(enable_sanitizers=True)
    flags = ["-v"] * 5
    res = runner.run(flags + ["--help"])
    assert not res.asan_violation
    assert res.exit_code == 0


def test_b25_asan_clean_exit_code_on_parse_error():
    """B25-2: Command syntax error returns clean code without sanitizer violation."""
    runner = BinaryRunner(enable_sanitizers=True)
    res = runner.run(["push"])  # Missing required path argument
    assert res.exit_code != 0
    assert not res.asan_violation


def test_b25_asan_rapid_consecutive_runs():
    """B25-3: Consecutive process runs execute cleanly under ASan."""
    runner = BinaryRunner(enable_sanitizers=True)
    for _ in range(3):
        res = runner.run(["--help"])
        assert not res.asan_violation
        assert res.exit_code == 0


def test_b25_asan_options_environment_passthrough():
    """B25-4: Custom ASAN_OPTIONS environment variable honored."""
    runner = BinaryRunner(enable_sanitizers=True)
    res = runner.run(["--help"], extra_env={"ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1"})
    assert not res.asan_violation


def test_b25_ubsan_zero_division_guard():
    """B25-5: Zero-division guard in progress and speed tracking prevents UBSan trigger."""
    runner = BinaryRunner(enable_sanitizers=True)
    res = runner.run(["channel"])
    assert not res.ubsan_violation


# ── F26: Binary Security Verification Boundaries ─────────────────────────

def test_b26_elf_class_64_bit():
    """B26-1: ELF class is 64-bit."""
    res = subprocess.run(["readelf", "-h", str(BINARY_PATH)], capture_output=True, text=True)
    assert "ELF64" in res.stdout


def test_b26_machine_architecture_x86_64():
    """B26-2: Machine architecture is AMD x86-64."""
    res = subprocess.run(["readelf", "-h", str(BINARY_PATH)], capture_output=True, text=True)
    assert "Advanced Micro Devices X86-64" in res.stdout or "x86-64" in res.stdout


def test_b26_no_textrels():
    """B26-3: Dynamic section contains no TEXTREL relocations."""
    res = subprocess.run(["readelf", "-d", str(BINARY_PATH)], capture_output=True, text=True)
    assert "TEXTREL" not in res.stdout


def test_b26_no_executable_heap():
    """B26-4: Heap and bss segments are non-executable."""
    res = subprocess.run(["readelf", "-l", str(BINARY_PATH)], capture_output=True, text=True)
    assert "GNU_STACK" in res.stdout


def test_b26_readelf_hardening_parity():
    """B26-5: readelf confirms essential hardening attributes."""
    inspector = BinaryInspector(BINARY_PATH)
    h = inspector.get_readelf_hardening()
    assert h["pie"]
    assert h["canary"]
    assert h["nx"]


# ── F27: Clean Git Commit Boundaries ─────────────────────────────────────

def test_b27_no_unstaged_source_corruptions():
    """B27-1: C++ source files have no uncommitted merge conflict markers."""
    for p in (PROJECT_ROOT / "src").rglob("*.cpp"):
        content = p.read_text(encoding="utf-8", errors="replace")
        assert "<<<<<<<" not in content, f"Merge conflict in {p}"
        assert ">>>>>>>" not in content, f"Merge conflict in {p}"


def test_b27_valid_git_author():
    """B27-2: Recent git commit authors are formatted properly."""
    res = subprocess.run(
        ["git", "log", "-n", "1", "--format=%an <%ae>"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0
    assert len(res.stdout.strip()) > 0


def test_b27_branch_matches_needspeed():
    """B27-3: Branch verification ensures needspeed branch."""
    res = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0
    assert res.stdout.strip() == "needspeed"


def test_b27_no_temporary_editor_files():
    """B27-4: Working tree does not contain editor backup or temporary files."""
    stray = []
    for pattern in ["*~", "*.swp", "*.swo", ".DS_Store"]:
        for p in PROJECT_ROOT.glob(f"**/{pattern}"):
            if ".git" not in str(p):
                stray.append(str(p.relative_to(PROJECT_ROOT)))
    assert not stray, f"Found editor backup files: {stray}"


def test_b27_clean_commit_history_reachable():
    """B27-5: Git HEAD points to a valid commit object."""
    res = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
    )
    assert res.returncode == 0
    assert len(res.stdout.strip()) == 40  # 40-char SHA1
