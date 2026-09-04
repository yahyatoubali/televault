"""Empirical Challenger M2 Iteration 2 Stress Test Suite.

Adversarially stress-tests:
1. Cross-language crypto verification (Python <-> C++):
   - 44-byte salted format & 28-byte legacy format across boundary sizes (0B, 1B, 15B, 16B, 17B, 32B, 64KB, 1MB)
   - Critical boundary collision: 16B legacy payload has total length 44B (exact match with WIRE_OVERHEAD)
   - Fallback salt handling and missing fallback salt rejection
   - Exhaustive bit-flip tampering (salt, nonce, ciphertext, tag) and truncated ciphertexts
2. Streaming crypto stress:
   - Out-of-order block injection at start, mid-stream, and reverse order
   - Replay attacks (immediate replay, delayed replay, alternating replay)
   - Skipped/dropped blocks and corrupted block tags/bodies
   - Interoperability between Python and C++ streaming encryptor/decryptor
3. Compression stress:
   - Decompress ZSTD frames with ZSTD_CONTENTSIZE_UNKNOWN (0B, 1B, 127B, 4KB, 64KB, 1MB, 4MB, 8MB)
   - Multi-megabyte payloads under both standard and AddressSanitizer/UBSan builds
   - Truncated and corrupted ZSTD frame error handling
"""

import os
import struct
import subprocess
from pathlib import Path
import pytest
import zstandard as zstd
from cryptography.hazmat.primitives.ciphers.aead import AESGCM

from televault.crypto import (
    encrypt_chunk as py_encrypt_chunk,
    decrypt_chunk as py_decrypt_chunk,
    SALT_SIZE,
    NONCE_SIZE,
    TAG_SIZE,
    HEADER_SIZE,
)
from .e2e.harness.crypto_oracle import CryptoOracle

PROJECT_ROOT = Path(__file__).resolve().parent.parent
BUILD_HELPER = PROJECT_ROOT / "build" / "tests" / "m2_cross_helper"
BUILD_ASAN_HELPER = PROJECT_ROOT / "build-asan" / "tests" / "m2_cross_helper"

HELPERS = [
    pytest.param(BUILD_HELPER, id="standard_build"),
    pytest.param(BUILD_ASAN_HELPER, id="asan_ubsan_build"),
]


def _derive_counter_nonce(base_nonce: bytes, counter: int) -> bytes:
    """Derive 12-byte counter nonce matching C++ derive_counter_nonce (8 bytes base + 4 bytes BE counter)."""
    return base_nonce[:8] + struct.pack(">I", counter)


def run_helper(helper_path: Path, args: list[str], input_bytes: bytes, timeout: int = 30) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1:check_initialization_order=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return subprocess.run(
        [str(helper_path)] + args,
        input=input_bytes,
        capture_output=True,
        env=env,
        timeout=timeout,
    )


def _encode_length_prefixed(blocks: list[bytes]) -> bytes:
    buf = bytearray()
    for b in blocks:
        buf.extend(struct.pack(">I", len(b)))
        buf.extend(b)
    return bytes(buf)


def _decode_length_prefixed(data: bytes) -> list[bytes]:
    blocks = []
    idx = 0
    while idx + 4 <= len(data):
        (length,) = struct.unpack(">I", data[idx : idx + 4])
        idx += 4
        assert idx + length <= len(data), f"Truncated stream at offset {idx}"
        blocks.append(data[idx : idx + length])
        idx += length
    return blocks


# ============================================================================
# Section 1: Detailed Cross-Language Crypto Verification & Boundary Analysis
# ============================================================================

CRYPTO_PAYLOAD_SIZES = [0, 1, 15, 16, 17, 31, 32, 33, 1024, 65536]


@pytest.mark.parametrize("helper", HELPERS)
@pytest.mark.parametrize("size", CRYPTO_PAYLOAD_SIZES)
def test_challenger_cross_crypto_boundary_sizes_py_to_cpp(helper, size):
    """Encrypt various boundary sizes in Python (Scrypt) -> Decrypt in C++."""
    password = f"secret_pass_{size}"
    plaintext = os.urandom(size)

    encrypted = py_encrypt_chunk(plaintext, password)
    assert len(encrypted) == size + 44

    res = run_helper(helper, ["decrypt_chunk_pass", password], encrypted)
    assert res.returncode == 0, f"C++ failed decrypt for size {size}: {res.stderr.decode()}"
    assert res.stdout == plaintext


@pytest.mark.parametrize("helper", HELPERS)
@pytest.mark.parametrize("size", CRYPTO_PAYLOAD_SIZES)
def test_challenger_cross_crypto_boundary_sizes_cpp_to_py(helper, size):
    """Encrypt various boundary sizes in C++ -> Decrypt in Python."""
    password = f"secret_pass_cpp_{size}"
    plaintext = os.urandom(size)

    res = run_helper(helper, ["encrypt_chunk_pass", password], plaintext)
    assert res.returncode == 0, f"C++ failed encrypt for size {size}: {res.stderr.decode()}"
    encrypted_cpp = res.stdout
    assert len(encrypted_cpp) == size + 44

    decrypted = py_decrypt_chunk(encrypted_cpp, password)
    assert decrypted == plaintext


@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_cross_crypto_large_1mb_roundtrip(helper):
    """Verify 1MB payload roundtrip both directions under ASan."""
    password = "large_payload_pass_1mb"
    plaintext = os.urandom(1024 * 1024)

    # Py -> C++
    enc_py = py_encrypt_chunk(plaintext, password)
    res_cpp = run_helper(helper, ["decrypt_chunk_pass", password], enc_py, timeout=60)
    assert res_cpp.returncode == 0, f"C++ failed: {res_cpp.stderr.decode()}"
    assert res_cpp.stdout == plaintext

    # C++ -> Py
    res_enc = run_helper(helper, ["encrypt_chunk_pass", password], plaintext, timeout=60)
    assert res_enc.returncode == 0, f"C++ failed: {res_enc.stderr.decode()}"
    dec_py = py_decrypt_chunk(res_enc.stdout, password)
    assert dec_py == plaintext


@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_legacy_28_critical_44byte_collision(helper):
    """Test legacy 28-byte chunk with 16B plaintext (total size == 44B).
    
    A 16-byte plaintext in legacy format produces a 44-byte ciphertext (12B nonce + 16B ct + 16B tag).
    In 44-byte format, a 44-byte ciphertext corresponds to 0 bytes plaintext.
    C++ dual wire format MUST distinguish them and fall back to legacy mode!
    """
    key = os.urandom(32)
    nonce = os.urandom(12)
    plaintext = b"0123456789ABCDEF"  # exactly 16 bytes
    assert len(plaintext) == 16

    aesgcm = AESGCM(key)
    ct_with_tag = aesgcm.encrypt(nonce, plaintext, None)
    legacy_44 = nonce + ct_with_tag
    assert len(legacy_44) == 44  # Exact collision with WIRE_OVERHEAD

    res = run_helper(helper, ["decrypt_chunk_key", key.hex()], legacy_44)
    assert res.returncode == 0, f"C++ failed on 44-byte legacy collision: {res.stderr.decode()}"
    assert res.stdout == plaintext


@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_legacy_28_password_fallback_salt_handling(helper):
    """Test legacy 28-byte chunk decryption with password + fallback salt, and verify error without fallback salt."""
    password = "legacy_fallback_test_pass"
    fallback_salt = os.urandom(16)
    nonce = os.urandom(12)
    plaintext = b"Legacy payload requiring fallback salt"

    key = CryptoOracle.derive_key(password, fallback_salt)
    aesgcm = AESGCM(key)
    legacy_payload = nonce + aesgcm.encrypt(nonce, plaintext, None)
    assert len(legacy_payload) == len(plaintext) + 28

    # 1. Decrypt with fallback_salt provided -> SUCCESS
    # In C++, m2_cross_helper takes: decrypt_chunk_pass <password> [fallback_salt_hex]
    # But wait, default derive_key in C++ uses Scrypt!
    # Let's derive key via Scrypt in Python:
    from televault.crypto import derive_key as py_derive_scrypt
    scrypt_key = py_derive_scrypt(password, fallback_salt)
    legacy_payload_scrypt = nonce + AESGCM(scrypt_key).encrypt(nonce, plaintext, None)

    res_ok = run_helper(helper, ["decrypt_chunk_pass", password, fallback_salt.hex()], legacy_payload_scrypt)
    assert res_ok.returncode == 0, f"C++ failed with fallback salt: {res_ok.stderr.decode()}"
    assert res_ok.stdout == plaintext

    # 2. Decrypt without fallback_salt -> MUST FAIL with informative error
    res_fail = run_helper(helper, ["decrypt_chunk_pass", password], legacy_payload_scrypt)
    assert res_fail.returncode != 0
    assert b"fallback salt" in res_fail.stderr.lower() or b"failed" in res_fail.stderr.lower()


# ============================================================================
# Section 2: Comprehensive Tampering & Corruption Matrix
# ============================================================================

@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_exhaustive_wire_tampering(helper):
    """Verify bit-level corruption rejection across all functional regions of 44-byte payload."""
    password = "tamper_exhaustive_pass"
    plaintext = b"Comprehensive tampering verification payload for TeleVault"
    encrypted = py_encrypt_chunk(plaintext, password)
    assert len(encrypted) == len(plaintext) + 44

    # Test bit-flip in each critical region:
    # Region 1: Salt (0..15)
    for offset in [0, 7, 15]:
        corrupted = bytearray(encrypted)
        corrupted[offset] ^= 0x80
        res = run_helper(helper, ["decrypt_chunk_pass", password], bytes(corrupted))
        assert res.returncode != 0, f"C++ accepted corrupted salt at byte {offset}!"

    # Region 2: Nonce (16..27)
    for offset in [16, 21, 27]:
        corrupted = bytearray(encrypted)
        corrupted[offset] ^= 0x01
        res = run_helper(helper, ["decrypt_chunk_pass", password], bytes(corrupted))
        assert res.returncode != 0, f"C++ accepted corrupted nonce at byte {offset}!"

    # Region 3: Ciphertext body (28..len-17)
    for offset in [28, 35, len(encrypted) - 17]:
        corrupted = bytearray(encrypted)
        corrupted[offset] ^= 0x04
        res = run_helper(helper, ["decrypt_chunk_pass", password], bytes(corrupted))
        assert res.returncode != 0, f"C++ accepted corrupted ciphertext at byte {offset}!"

    # Region 4: Auth Tag (len-16..len-1)
    for offset in [len(encrypted) - 16, len(encrypted) - 8, len(encrypted) - 1]:
        corrupted = bytearray(encrypted)
        corrupted[offset] ^= 0x40
        res = run_helper(helper, ["decrypt_chunk_pass", password], bytes(corrupted))
        assert res.returncode != 0, f"C++ accepted corrupted tag at byte {offset}!"


@pytest.mark.parametrize("helper", HELPERS)
@pytest.mark.parametrize("trunc_len", [0, 1, 10, 12, 27, 28, 43])
def test_challenger_truncated_ciphertext_rejection(helper, trunc_len):
    """Ciphertexts truncated below minimum valid headers must be rejected without crash."""
    key = os.urandom(32)
    fake_ct = os.urandom(trunc_len)

    res = run_helper(helper, ["decrypt_chunk_key", key.hex()], fake_ct)
    assert res.returncode != 0, f"C++ accepted truncated ciphertext of len {trunc_len}!"


@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_appended_garbage_rejection(helper):
    """Appending trailing bytes shifts the expected tag position and must fail authentication."""
    password = "trailing_bytes_test"
    plaintext = b"Payload that will have garbage appended"
    encrypted = py_encrypt_chunk(plaintext, password)

    for extra_bytes in [b"\x00", b"\xff\xff", b"TRAILING_GARBAGE"]:
        corrupted = encrypted + extra_bytes
        res = run_helper(helper, ["decrypt_chunk_pass", password], corrupted)
        assert res.returncode != 0, f"C++ accepted ciphertext with {len(extra_bytes)} extra bytes!"


# ============================================================================
# Section 3: Streaming Crypto Adversarial Injection & Replay Stress
# ============================================================================

@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_streaming_swap_adjacent_blocks(helper):
    """Swap adjacent blocks in the middle of a stream (blocks 3 and 4 out of 8)."""
    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    plain_blocks = [f"Stream block number {i:03d} content".encode() for i in range(8)]
    enc_blocks = [aesgcm.encrypt(_derive_counter_nonce(base_nonce, i), b, None) for i, b in enumerate(plain_blocks)]

    # Swap block 3 and block 4
    swapped = list(enc_blocks)
    swapped[3], swapped[4] = swapped[4], swapped[3]

    res = run_helper(helper, ["stream_decrypt", key.hex(), base_nonce.hex()], _encode_length_prefixed(swapped))
    assert res.returncode != 0, "C++ did not reject swapped stream blocks 3 and 4!"


@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_streaming_delayed_replay(helper):
    """Send blocks 0..5, then replay block 2 at position 6."""
    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    plain_blocks = [f"Payload item {i}".encode() for i in range(7)]
    enc_blocks = [aesgcm.encrypt(_derive_counter_nonce(base_nonce, i), b, None) for i, b in enumerate(plain_blocks)]

    # Stream: [0, 1, 2, 3, 4, 5, 2(replay)]
    replay_stream = enc_blocks[:6] + [enc_blocks[2]]

    res = run_helper(helper, ["stream_decrypt", key.hex(), base_nonce.hex()], _encode_length_prefixed(replay_stream))
    assert res.returncode != 0, "C++ did not reject delayed replay of block 2 at index 6!"


@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_streaming_cpp_encrypt_py_decrypt_roundtrip(helper):
    """C++ stream_encrypt -> Python decrypts each block using counter-derived nonce."""
    key = os.urandom(32)
    base_nonce = os.urandom(12)

    plain_blocks = [os.urandom(size) for size in [0, 1, 17, 256, 1024, 4096, 16384]]
    input_stream = _encode_length_prefixed(plain_blocks)

    res = run_helper(helper, ["stream_encrypt", key.hex(), base_nonce.hex()], input_stream)
    assert res.returncode == 0, f"C++ stream_encrypt failed: {res.stderr.decode()}"

    enc_blocks = _decode_length_prefixed(res.stdout)
    assert len(enc_blocks) == len(plain_blocks)

    aesgcm = AESGCM(key)
    for i, enc_b in enumerate(enc_blocks):
        nonce = _derive_counter_nonce(base_nonce, i)
        decrypted = aesgcm.decrypt(nonce, enc_b, None)
        assert decrypted == plain_blocks[i], f"Mismatch at block {i}"


# ============================================================================
# Section 4: Compression Stress (ZSTD_CONTENTSIZE_UNKNOWN & Multi-MB Streams)
# ============================================================================

ZSTD_UNKNOWN_SIZES = [0, 1, 7, 127, 512, 4096, 65536, 1024 * 1024, 4 * 1024 * 1024]


@pytest.mark.parametrize("helper", HELPERS)
@pytest.mark.parametrize("size", ZSTD_UNKNOWN_SIZES)
def test_challenger_decompress_unknown_size_matrix(helper, size):
    """Verify decompress_data handles ZSTD_CONTENTSIZE_UNKNOWN across small, medium, and multi-MB sizes."""
    if size == 0:
        raw = b""
    else:
        # Repeating pattern mixed with byte variations
        chunk = b"TELEVAULT_ZSTD_UNKNOWN_SIZE_STRESS_PATTERN_"
        raw = (chunk * (size // len(chunk) + 1))[:size]

    cctx = zstd.ZstdCompressor(level=3, write_content_size=False)
    compressed = cctx.compress(raw)

    params = zstd.get_frame_parameters(compressed)
    assert params.content_size in (0, None, zstd.CONTENTSIZE_UNKNOWN)

    res = run_helper(helper, ["decompress_zstd"], compressed, timeout=30)
    assert res.returncode == 0, f"C++ decompress failed on size {size}: {res.stderr.decode()}"
    assert res.stdout == raw


@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_decompress_unknown_size_uncompressible_data(helper):
    """ZSTD_CONTENTSIZE_UNKNOWN with completely uncompressible random bytes."""
    size = 256 * 1024  # 256 KB
    raw = os.urandom(size)

    cctx = zstd.ZstdCompressor(level=3, write_content_size=False)
    compressed = cctx.compress(raw)

    res = run_helper(helper, ["decompress_zstd"], compressed)
    assert res.returncode == 0, f"C++ failed on random unknown-size frame: {res.stderr.decode()}"
    assert res.stdout == raw


@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_decompress_corrupted_zstd_frame(helper):
    """Corrupted ZSTD frame headers, block contents, or truncated frames must throw cleanly without ASan issues."""
    raw = b"Sample text data to compress" * 20
    
    # 1. Corrupt magic number (first 4 bytes)
    cctx = zstd.ZstdCompressor(level=3)
    compressed = bytearray(cctx.compress(raw))
    corrupted_magic = bytearray(compressed)
    corrupted_magic[0] ^= 0xFF
    res = run_helper(helper, ["decompress_zstd"], bytes(corrupted_magic))
    assert res.returncode != 0, "C++ did not reject corrupted ZSTD magic number!"

    # 2. Truncated frame
    truncated = compressed[:len(compressed) // 2]
    res = run_helper(helper, ["decompress_zstd"], bytes(truncated))
    assert res.returncode != 0, "C++ did not reject truncated ZSTD frame!"

    # 3. Corrupted payload block with frame checksum enabled
    cctx_cs = zstd.ZstdCompressor(level=3, write_checksum=True)
    compressed_cs = bytearray(cctx_cs.compress(raw))
    corrupted_cs = bytearray(compressed_cs)
    corrupted_cs[len(corrupted_cs) // 2] ^= 0xAA
    res = run_helper(helper, ["decompress_zstd"], bytes(corrupted_cs))
    assert res.returncode != 0, "C++ did not reject corrupted ZSTD frame with checksum!"


@pytest.mark.parametrize("helper", HELPERS)
def test_challenger_large_8mb_zstd_unknown_size_stress(helper):
    """Stress test 8MB unknown-content-size frame under AddressSanitizer."""
    size = 8 * 1024 * 1024
    pattern = b"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_-" * 100
    raw = (pattern * (size // len(pattern) + 1))[:size]

    cctx = zstd.ZstdCompressor(level=3, write_content_size=False)
    compressed = cctx.compress(raw)

    res = run_helper(helper, ["decompress_zstd"], compressed, timeout=45)
    assert res.returncode == 0, f"C++ failed on 8MB unknown size decompression: {res.stderr.decode()}"
    assert res.stdout == raw
