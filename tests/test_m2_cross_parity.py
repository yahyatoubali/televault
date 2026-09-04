"""Cross-Language Empirical Stress Harness for Milestone 2.

Validates parity between Python (CryptoOracle / src/televault/crypto.py / zstandard)
and C++ (tv_core / m2_cross_helper) across both standard and ASan/UBSan builds.
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
    derive_key as py_derive_scrypt,
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
    """Derive 12-byte counter nonce matching C++ derive_counter_nonce."""
    return base_nonce[:8] + struct.pack(">I", counter)


def run_helper(helper_path: Path, args: list[str], input_bytes: bytes) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return subprocess.run(
        [str(helper_path)] + args,
        input=input_bytes,
        capture_output=True,
        env=env,
        timeout=30,
    )


# ============================================================================
# 1. Cross-Language Crypto Verification
# ============================================================================

@pytest.mark.parametrize("helper", HELPERS)
def test_cross_python_encrypt_scrypt_cpp_decrypt(helper):
    """Encrypt in Python (src/televault/crypto.py with Scrypt) -> Decrypt in C++."""
    password = "cross_lang_test_secret_pass_123"
    plaintext = b"Cross-Language Plaintext Data from Python to C++ via Scrypt!"
    
    # Python produces 44-byte format: salt(16) + nonce(12) + ct + tag(16)
    encrypted_py = py_encrypt_chunk(plaintext, password)
    assert len(encrypted_py) == len(plaintext) + 44

    res = run_helper(helper, ["decrypt_chunk_pass", password], encrypted_py)
    assert res.returncode == 0, f"C++ decryption failed: {res.stderr.decode()}"
    assert res.stdout == plaintext


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_cpp_encrypt_scrypt_python_decrypt(helper):
    """Encrypt in C++ -> Decrypt in Python (src/televault/crypto.py)."""
    password = "cross_lang_test_secret_pass_456"
    salt = os.urandom(16)
    plaintext = b"Cross-Language Plaintext Data from C++ to Python via Scrypt!"

    res = run_helper(helper, ["encrypt_chunk_pass", password, salt.hex()], plaintext)
    assert res.returncode == 0, f"C++ encryption failed: {res.stderr.decode()}"
    encrypted_cpp = res.stdout
    assert len(encrypted_cpp) == len(plaintext) + 44

    decrypted_py = py_decrypt_chunk(encrypted_cpp, password)
    assert decrypted_py == plaintext


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_python_encrypt_raw_key_44_cpp_decrypt(helper):
    """Python raw key AES-GCM (44-byte wire format) -> C++ decrypt_chunk_key."""
    key = os.urandom(32)
    salt = os.urandom(16)
    nonce = os.urandom(12)
    plaintext = b"Raw 32-byte key encryption 44-byte wire format"

    aesgcm = AESGCM(key)
    ct_with_tag = aesgcm.encrypt(nonce, plaintext, None)
    payload = salt + nonce + ct_with_tag
    assert len(payload) == len(plaintext) + 44

    res = run_helper(helper, ["decrypt_chunk_key", key.hex()], payload)
    assert res.returncode == 0, f"C++ decrypt_chunk_key failed: {res.stderr.decode()}"
    assert res.stdout == plaintext


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_python_encrypt_legacy_28_cpp_decrypt(helper):
    """Python raw key AES-GCM (legacy 28-byte wire format: nonce + ct + tag) -> C++ fallback decrypt."""
    key = os.urandom(32)
    nonce = os.urandom(12)
    plaintext = b"Legacy 28-byte wire format payload without embedded salt"

    aesgcm = AESGCM(key)
    ct_with_tag = aesgcm.encrypt(nonce, plaintext, None)
    legacy_payload = nonce + ct_with_tag
    assert len(legacy_payload) == len(plaintext) + 28

    res = run_helper(helper, ["decrypt_chunk_key", key.hex()], legacy_payload)
    assert res.returncode == 0, f"C++ legacy decrypt failed: {res.stderr.decode()}"
    assert res.stdout == plaintext


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_cpp_encrypt_raw_key_44_python_decrypt(helper):
    """C++ encrypt_chunk_key (44-byte format) -> Python AESGCM decrypt."""
    key = os.urandom(32)
    salt = os.urandom(16)
    plaintext = b"Plaintext encrypted by C++ raw key to be decrypted by Python"

    res = run_helper(helper, ["encrypt_chunk_key", key.hex(), salt.hex()], plaintext)
    assert res.returncode == 0, f"C++ encrypt failed: {res.stderr.decode()}"
    ct44 = res.stdout
    assert len(ct44) == len(plaintext) + 44

    extracted_salt = ct44[:16]
    assert extracted_salt == salt
    extracted_nonce = ct44[16:28]
    ct_with_tag = ct44[28:]

    aesgcm = AESGCM(key)
    decrypted = aesgcm.decrypt(extracted_nonce, ct_with_tag, None)
    assert decrypted == plaintext


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_crypto_oracle_pbkdf2_parity(helper):
    """CryptoOracle (PBKDF2-HMAC-SHA256) encrypted chunk -> C++ decrypt."""
    password = "oracle_password_789"
    plaintext = b"CryptoOracle cross test message"
    salt = os.urandom(16)
    key = CryptoOracle.derive_key(password, salt)

    ct44 = CryptoOracle.encrypt_chunk_44(plaintext, password, salt=salt)
    res = run_helper(helper, ["decrypt_chunk_key", key.hex()], ct44)
    assert res.returncode == 0, f"C++ failed to decrypt Oracle chunk: {res.stderr.decode()}"
    assert res.stdout == plaintext


# ============================================================================
# 2. Cross-Language Tamper Rejection
# ============================================================================

@pytest.mark.parametrize("helper", HELPERS)
def test_cross_tampered_tag_rejection_cpp(helper):
    """Corrupted authentication tag in Python payload must be rejected by C++."""
    password = "tamper_test_pass"
    plaintext = b"Data with tag to tamper"
    encrypted = bytearray(py_encrypt_chunk(plaintext, password))
    
    # Flip bit in auth tag (last 16 bytes)
    encrypted[-1] ^= 0x01

    res = run_helper(helper, ["decrypt_chunk_pass", password], bytes(encrypted))
    assert res.returncode != 0, "C++ did not reject tampered authentication tag!"


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_tampered_ciphertext_rejection_cpp(helper):
    """Corrupted ciphertext byte in Python payload must be rejected by C++."""
    password = "tamper_test_pass"
    plaintext = b"Data with ciphertext to tamper"
    encrypted = bytearray(py_encrypt_chunk(plaintext, password))
    
    # Flip bit in ciphertext body
    encrypted[HEADER_SIZE + 2] ^= 0x01

    res = run_helper(helper, ["decrypt_chunk_pass", password], bytes(encrypted))
    assert res.returncode != 0, "C++ did not reject tampered ciphertext!"


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_tampered_salt_rejection_cpp(helper):
    """Corrupted salt in password-derived payload must fail C++ authentication."""
    password = "tamper_test_pass"
    plaintext = b"Data with salt to tamper"
    encrypted = bytearray(py_encrypt_chunk(plaintext, password))
    
    # Flip bit in salt (first 16 bytes)
    encrypted[0] ^= 0x01

    res = run_helper(helper, ["decrypt_chunk_pass", password], bytes(encrypted))
    assert res.returncode != 0, "C++ did not reject tampered salt in password mode!"


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_tampered_cpp_payload_rejected_by_python(helper):
    """Payload encrypted by C++ and tampered must be rejected by Python."""
    password = "tamper_test_pass"
    plaintext = b"Payload from C++ to tamper"

    res = run_helper(helper, ["encrypt_chunk_pass", password], plaintext)
    assert res.returncode == 0
    tampered = bytearray(res.stdout)
    tampered[-1] ^= 0x01  # Corrupt tag

    with pytest.raises(Exception):
        py_decrypt_chunk(bytes(tampered), password)


# ============================================================================
# 3. Streaming Crypto Stress: Out-of-Order & Replay Attacks Cross-Language
# ============================================================================

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


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_streaming_crypto_roundtrip(helper):
    """Python encrypts stream blocks -> C++ decrypts stream blocks."""
    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    plain_blocks = [f"Streaming Block #{i} data content".encode() * 10 for i in range(8)]
    enc_blocks = []
    for i, b in enumerate(plain_blocks):
        nonce = _derive_counter_nonce(base_nonce, i)
        ct = aesgcm.encrypt(nonce, b, None)
        enc_blocks.append(ct)

    input_stream = _encode_length_prefixed(enc_blocks)
    res = run_helper(helper, ["stream_decrypt", key.hex(), base_nonce.hex()], input_stream)
    assert res.returncode == 0, f"C++ streaming decrypt failed: {res.stderr.decode()}"

    decrypted_blocks = _decode_length_prefixed(res.stdout)
    assert decrypted_blocks == plain_blocks


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_streaming_out_of_order_block_rejected(helper):
    """Out-of-order block injection sent to C++ StreamingDecryptor must fail."""
    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    b0 = b"First in-order block"
    b1 = b"Second block out of order"

    eb0 = aesgcm.encrypt(_derive_counter_nonce(base_nonce, 0), b0, None)
    eb1 = aesgcm.encrypt(_derive_counter_nonce(base_nonce, 1), b1, None)

    # Attack: send block 1 FIRST
    corrupted_stream = _encode_length_prefixed([eb1, eb0])
    res = run_helper(helper, ["stream_decrypt", key.hex(), base_nonce.hex()], corrupted_stream)
    assert res.returncode != 0, "C++ did not reject out-of-order block 1 injected at counter 0!"


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_streaming_replay_attack_rejected(helper):
    """Replay of block 0 after block 0 to C++ StreamingDecryptor must fail."""
    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    b0 = b"Transaction payload: Transfer $100"
    b1 = b"Transaction payload: Transfer $200"

    eb0 = aesgcm.encrypt(_derive_counter_nonce(base_nonce, 0), b0, None)
    eb1 = aesgcm.encrypt(_derive_counter_nonce(base_nonce, 1), b1, None)

    # Attack: send block 0, then replay block 0 instead of block 1
    replay_stream = _encode_length_prefixed([eb0, eb0])
    res = run_helper(helper, ["stream_decrypt", key.hex(), base_nonce.hex()], replay_stream)
    assert res.returncode != 0, "C++ did not reject replayed block 0 at counter 1!"


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_streaming_skipped_block_rejected(helper):
    """Skipping a block (send block 0 then block 2) must fail authentication in C++."""
    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    b0 = b"Block 0"
    b2 = b"Block 2"

    eb0 = aesgcm.encrypt(_derive_counter_nonce(base_nonce, 0), b0, None)
    eb2 = aesgcm.encrypt(_derive_counter_nonce(base_nonce, 2), b2, None)

    skip_stream = _encode_length_prefixed([eb0, eb2])
    res = run_helper(helper, ["stream_decrypt", key.hex(), base_nonce.hex()], skip_stream)
    assert res.returncode != 0, "C++ did not reject skipped block 2 at counter 1!"


# ============================================================================
# 4. Compression Stress: ZSTD_CONTENTSIZE_UNKNOWN & Multi-MB Streams
# ============================================================================

@pytest.mark.parametrize("helper", HELPERS)
@pytest.mark.parametrize("payload_size", [128, 4096, 65536, 1024 * 1024])
def test_cross_decompress_unknown_content_size(helper, payload_size):
    """Python compresses without writing content size -> C++ decompress_data handles it."""
    # Pattern repeated
    raw_data = (b"TeleVault compression parity stress test payload! \x00\xff" * (payload_size // 40 + 1))[:payload_size]

    # Compress in Python with write_content_size=False
    cctx = zstd.ZstdCompressor(level=3, write_content_size=False)
    compressed_py = cctx.compress(raw_data)

    # Confirm frame header in Python has unknown content size
    params = zstd.get_frame_parameters(compressed_py)
    assert params.content_size == 0 or params.content_size is None or params.content_size == zstd.CONTENTSIZE_UNKNOWN

    res = run_helper(helper, ["decompress_zstd"], compressed_py)
    assert res.returncode == 0, f"C++ decompress_zstd failed: {res.stderr.decode()}"
    assert res.stdout == raw_data


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_cpp_compress_unknown_size_python_decompress(helper):
    """C++ compresses with ZSTD_c_contentSizeFlag=0 -> Python decompressor handles it."""
    payload_size = 512 * 1024  # 512KB
    raw_data = os.urandom(payload_size)

    res = run_helper(helper, ["compress_zstd_unknown_size"], raw_data)
    assert res.returncode == 0, f"C++ compress failed: {res.stderr.decode()}"
    compressed_cpp = res.stdout

    # Verify content size is unknown
    params = zstd.get_frame_parameters(compressed_cpp)
    assert params.content_size == 0 or params.content_size is None or params.content_size == zstd.CONTENTSIZE_UNKNOWN

    dctx = zstd.ZstdDecompressor()
    decompressed = dctx.decompress(compressed_cpp, max_output_size=payload_size * 2)
    assert decompressed == raw_data


@pytest.mark.parametrize("helper", HELPERS)
def test_cross_multi_megabyte_compression_stress(helper):
    """Stress test 5MB payload compression/decompression across languages."""
    payload_size = 5 * 1024 * 1024  # 5MB
    # Mix of repetitive text and random bytes
    chunk = b"TELEVAULT_MULTI_MEGABYTE_STRESS_CHALLENGE_" * 100
    raw_data = (chunk * (payload_size // len(chunk) + 1))[:payload_size]

    # Python compresses
    cctx = zstd.ZstdCompressor(level=3, write_content_size=False)
    compressed = cctx.compress(raw_data)

    # C++ decompresses under memory pressure
    res = run_helper(helper, ["decompress_zstd"], compressed)
    assert res.returncode == 0, f"C++ failed on 5MB decompression: {res.stderr.decode()}"
    assert res.stdout == raw_data
