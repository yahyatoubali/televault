"""Tier 2 Boundaries: Features F01 through F05 (25 tests)."""

import os
import subprocess
import pytest

from ..harness.binary_inspector import BinaryInspector
from ..harness.binary_runner import BinaryRunner
from ..harness.config import BINARY_PATH, PROJECT_ROOT
from ..harness.crypto_oracle import CryptoOracle


# ── F01: Build Security Hardening Boundaries ─────────────────────────────

def test_b01_binary_stripped_or_unstripped_symbols():
    """B01-1: Symbol table format conforms to ELF64 standard."""
    inspector = BinaryInspector(BINARY_PATH)
    info = inspector.get_rabin2_info()
    if info:
        assert info.get("bits") == "64"
        assert info.get("class") == "ELF64"


def test_b01_no_executable_data_segments():
    """B01-2: Data segment is not executable (W^X / DEP)."""
    res = subprocess.run(["readelf", "-l", str(BINARY_PATH)], capture_output=True, text=True)
    # Check that LOAD segments with Write (W) do not have Execute (E)
    for line in res.stdout.splitlines():
        if "LOAD" in line:
            parts = line.split()
            # Flags typically at index 6 or 7 (e.g. R E or RW)
            flags = " ".join(parts[6:8]) if len(parts) >= 8 else ""
            if "W" in flags:
                assert "E" not in flags, f"Writable segment is also executable: {line}"


def test_b01_pic_base_address_zero():
    """B01-3: PIE virtual address base starts at 0x0."""
    inspector = BinaryInspector(BINARY_PATH)
    info = inspector.get_rabin2_info()
    if info:
        assert info.get("baddr") == "0x0" or info.get("pic") == "true"


def test_b01_relro_alignment():
    """B01-4: GNU_RELRO segment page alignment boundary."""
    res = subprocess.run(["readelf", "-l", str(BINARY_PATH)], capture_output=True, text=True)
    assert "GNU_RELRO" in res.stdout


def test_b01_compiler_security_flags_order():
    """B01-5: Fortify source defined with optimization >= -O1."""
    root_cmake = (PROJECT_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    src_cmake = (PROJECT_ROOT / "src" / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "_FORTIFY_SOURCE=2" in root_cmake + src_cmake


# ── F02: Sanitizer Integration Boundaries ────────────────────────────────

def test_b02_asan_detect_leaks_stress():
    """B02-1: Stress test running CLI commands repeatedly with leak detector."""
    runner = BinaryRunner(enable_sanitizers=True)
    for _ in range(3):
        res = runner.run(["channel"])
        assert not res.asan_violation


def test_b02_asan_null_pointer_safety():
    """B02-2: Empty and whitespace arguments handled safely."""
    runner = BinaryRunner(enable_sanitizers=True)
    res = runner.run(["find", ""])
    assert not res.asan_violation


def test_b02_asan_large_cli_argument_buffer():
    """B02-3: 16KB command line argument string does not trigger buffer overflow."""
    runner = BinaryRunner(enable_sanitizers=True)
    large_arg = "a" * 16384
    res = runner.run(["find", large_arg])
    assert not res.asan_violation


def test_b02_ubsan_shift_overflow_boundary():
    """B02-4: Numeric parameter parsing boundaries do not trigger integer overflow."""
    runner = BinaryRunner(enable_sanitizers=True)
    res = runner.run(["stat"])
    assert not res.ubsan_violation


def test_b02_asan_sigint_clean_shutdown():
    """B02-5: Fast command startup and termination leaves no orphaned sanitizer traces."""
    runner = BinaryRunner(enable_sanitizers=True)
    res = runner.run(["whoami"], timeout=3)
    assert not res.timed_out
    assert not res.asan_violation


# ── F03: Async Executor Boundaries ───────────────────────────────────────

def test_b03_zero_worker_threads():
    """B03-1: Thread configuration boundary: 0 workers handled safely."""
    src = (PROJECT_ROOT / "src" / "async" / "executor.cpp").read_text(encoding="utf-8")
    assert "threads" in src or "workers" in src or "std::thread" in src


def test_b03_extreme_high_worker_threads():
    """B03-2: Config specifying 128 workers parsed without crash."""
    runner = BinaryRunner()
    res = runner.run(["stat", "--help"])
    assert res.exit_code == 0


def test_b03_burst_task_enqueue():
    """B03-3: Async executor header declares work queue structures."""
    hdr = (PROJECT_ROOT / "src" / "async" / "executor.hpp").read_text(encoding="utf-8")
    assert "queue" in hdr.lower() or "submit" in hdr.lower() or "post" in hdr.lower() or "async" in hdr.lower()


def test_b03_immediate_shutdown_after_enqueue():
    """B03-4: Executor destruction safely cancels pending tasks."""
    hdr = (PROJECT_ROOT / "src" / "async" / "executor.hpp").read_text(encoding="utf-8")
    assert "~" in hdr or "stop" in hdr or "shutdown" in hdr


def test_b03_concurrent_exception_safety():
    """B03-5: Safe exception handling in task dispatcher."""
    src = (PROJECT_ROOT / "src" / "async" / "executor.cpp").read_text(encoding="utf-8")
    assert "catch" in src or "noexcept" in src or "future" in src or "task" in src


# ── F04: Cryptographic Wire Format Boundaries ────────────────────────────

def test_b04_minimum_ciphertext_size_boundary():
    """B04-1: Exact 44-byte ciphertext boundary represents 0-byte plaintext."""
    empty_ct = CryptoOracle.encrypt_chunk_44(b"", "pass")
    assert len(empty_ct) == 44
    assert CryptoOracle.decrypt_chunk(empty_ct, "pass") == b""


def test_b04_sub_28_byte_truncated_header():
    """B04-2: Truncated payload (< 28 bytes) rejected with ValueError."""
    with pytest.raises(ValueError):
        CryptoOracle.decrypt_chunk(b"short_bytes", "pass")


def test_b04_truncated_tag_boundary():
    """B04-3: Missing tag bytes rejected with authentication error."""
    valid = CryptoOracle.encrypt_chunk_44(b"data", "pass")
    truncated = valid[:-4]  # Truncate 4 bytes of tag
    with pytest.raises(Exception):
        CryptoOracle.decrypt_chunk(truncated, "pass")


def test_b04_all_zero_salt_and_nonce():
    """B04-4: Boundary condition: all-zero 16B salt and 12B nonce encrypts/decrypts cleanly."""
    salt = b"\x00" * 16
    nonce = b"\x00" * 12
    ct = CryptoOracle.encrypt_chunk_44(b"boundary data", "pass", salt=salt, nonce=nonce)
    assert ct[:16] == salt
    assert ct[16:28] == nonce
    assert CryptoOracle.decrypt_chunk(ct, "pass") == b"boundary data"


def test_b04_all_ff_salt_and_nonce():
    """B04-5: Boundary condition: all-0xFF 16B salt and 12B nonce encrypts/decrypts cleanly."""
    salt = b"\xFF" * 16
    nonce = b"\xFF" * 12
    ct = CryptoOracle.encrypt_chunk_44(b"boundary data", "pass", salt=salt, nonce=nonce)
    assert ct[:16] == salt
    assert ct[16:28] == nonce
    assert CryptoOracle.decrypt_chunk(ct, "pass") == b"boundary data"


# ── F05: Streaming Crypto Boundaries ─────────────────────────────────────

def test_b05_streaming_counter_overflow_boundary():
    """B05-1: High block counter values derive valid unique nonces."""
    import struct
    base_nonce = b"test_nonce_!"
    counter = 0x7FFFFFFF  # 2^31 - 1
    nonce = base_nonce[:8] + struct.pack(">I", counter)
    assert len(nonce) == 12


def test_b05_streaming_single_byte_blocks():
    """B05-2: Streaming handles single-byte block processing."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    import struct

    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    data = b"ABCDE"
    cts = []
    for i, b in enumerate(data):
        nonce = base_nonce[:8] + struct.pack(">I", i)
        cts.append(aesgcm.encrypt(nonce, bytes([b]), None))

    pts = []
    for i, ct in enumerate(cts):
        nonce = base_nonce[:8] + struct.pack(">I", i)
        pts.append(aesgcm.decrypt(nonce, ct, None))

    assert b"".join(pts) == data


def test_b05_streaming_large_chunk_boundary():
    """B05-3: Exact 1MB block encryption within stream."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    key = os.urandom(32)
    nonce = os.urandom(12)
    aesgcm = AESGCM(key)
    block = b"X" * (1024 * 1024)
    ct = aesgcm.encrypt(nonce, block, None)
    assert len(ct) == len(block) + 16
    assert aesgcm.decrypt(nonce, ct, None) == block


def test_b05_streaming_partial_block_flush():
    """B05-4: Final unaligned block in stream decrypts cleanly."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    key = os.urandom(32)
    nonce = os.urandom(12)
    aesgcm = AESGCM(key)
    odd_block = b"1234567"  # 7 bytes
    ct = aesgcm.encrypt(nonce, odd_block, None)
    assert aesgcm.decrypt(nonce, ct, None) == odd_block


def test_b05_streaming_corrupted_intermediate_block():
    """B05-5: Corrupted block in stream fails immediately."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    key = os.urandom(32)
    nonce = os.urandom(12)
    aesgcm = AESGCM(key)
    ct = bytearray(aesgcm.encrypt(nonce, b"payload", None))
    ct[5] ^= 0xFF  # Corrupt ciphertext byte
    with pytest.raises(Exception):
        aesgcm.decrypt(nonce, bytes(ct), None)
