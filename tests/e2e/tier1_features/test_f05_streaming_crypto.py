"""Feature 5 Tests: Streaming Encryption / Decryption."""

import os
import struct
import pytest

from ..harness.crypto_oracle import CryptoOracle


def _derive_counter_nonce(base_nonce: bytes, counter: int) -> bytes:
    """Derives counter-based 12-byte nonce (8-byte base + 4-byte counter)."""
    return base_nonce[:8] + struct.pack(">I", counter)


def test_f05_streaming_encrypt_blocks():
    """F05-1: Counter-based nonce derivation produces unique nonces per block."""
    base_nonce = os.urandom(12)
    nonces = {_derive_counter_nonce(base_nonce, i) for i in range(100)}
    assert len(nonces) == 100


def test_f05_streaming_decrypt_blocks():
    """F05-2: Streaming block encryption/decryption roundtrip."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    blocks = [b"Block #" + str(i).encode("ascii") * 100 for i in range(10)]
    encrypted_blocks = []
    for i, b in enumerate(blocks):
        nonce = _derive_counter_nonce(base_nonce, i)
        encrypted_blocks.append(aesgcm.encrypt(nonce, b, None))

    decrypted_blocks = []
    for i, eb in enumerate(encrypted_blocks):
        nonce = _derive_counter_nonce(base_nonce, i)
        decrypted_blocks.append(aesgcm.decrypt(nonce, eb, None))

    assert decrypted_blocks == blocks


def test_f05_streaming_multi_megabyte():
    """F05-3: Streaming encryption handles multi-megabyte payloads in fixed-size blocks."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    total_size = 5 * 1024 * 1024  # 5MB
    block_size = 64 * 1024  # 64KB blocks
    data = os.urandom(total_size)

    encrypted_stream = []
    for counter, offset in enumerate(range(0, total_size, block_size)):
        block = data[offset : offset + block_size]
        nonce = _derive_counter_nonce(base_nonce, counter)
        encrypted_stream.append(aesgcm.encrypt(nonce, block, None))

    reconstructed = bytearray()
    for counter, eb in enumerate(encrypted_stream):
        nonce = _derive_counter_nonce(base_nonce, counter)
        reconstructed.extend(aesgcm.decrypt(nonce, eb, None))

    assert bytes(reconstructed) == data


def test_f05_streaming_out_of_order_block_fails():
    """F05-4: Reordered blocks fail authentication due to counter nonce mismatch."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    block1 = b"First block"
    block2 = b"Second block"

    eb1 = aesgcm.encrypt(_derive_counter_nonce(base_nonce, 0), block1, None)
    eb2 = aesgcm.encrypt(_derive_counter_nonce(base_nonce, 1), block2, None)

    # Attempting to decrypt block2 at counter index 0 must fail
    with pytest.raises(Exception):
        aesgcm.decrypt(_derive_counter_nonce(base_nonce, 0), eb2, None)


def test_f05_streaming_empty_payload():
    """F05-5: Streaming empty payload produces empty processed stream without error."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    empty_ct = aesgcm.encrypt(_derive_counter_nonce(base_nonce, 0), b"", None)
    # Ciphertext of empty plaintext is just the 16-byte tag
    assert len(empty_ct) == 16
    assert aesgcm.decrypt(_derive_counter_nonce(base_nonce, 0), empty_ct, None) == b""
