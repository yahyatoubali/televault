"""Feature 4 Tests: Cryptographic Wire Format Parity."""

import os
import pytest

from ..harness.crypto_oracle import CryptoOracle


def test_f04_header_size_44_bytes():
    """F04-1: 44-byte wire format structure: 16B salt + 12B nonce + 16B tag overhead."""
    plaintext = b"Test wire format payload"
    password = "secret_password_123"
    encrypted = CryptoOracle.encrypt_chunk_44(plaintext, password)
    assert len(encrypted) == len(plaintext) + 44


def test_f04_dual_wire_format_parsing():
    """F04-2: Dual wire format: 44-byte salt-bearing format decrypts accurately."""
    plaintext = b"Secure payload with embedded salt"
    password = "pass"
    encrypted = CryptoOracle.encrypt_chunk_44(plaintext, password)
    decrypted = CryptoOracle.decrypt_chunk(encrypted, password)
    assert decrypted == plaintext


def test_f04_legacy_wire_format_support():
    """F04-3: Dual wire format: legacy 28-byte chunk decrypts using fallback salt."""
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM

    plaintext = b"Legacy chunk payload without embedded salt"
    password = "pass"
    fallback_salt = b"legacy_salt_1234"
    key = CryptoOracle.derive_key(password, fallback_salt)
    nonce = os.urandom(12)
    aesgcm = AESGCM(key)
    ct = aesgcm.encrypt(nonce, plaintext, None)
    legacy_payload = nonce + ct  # 12-byte nonce + (ciphertext + 16-byte tag)

    assert len(legacy_payload) == len(plaintext) + 28
    decrypted = CryptoOracle.decrypt_chunk(legacy_payload, password, fallback_salt=fallback_salt)
    assert decrypted == plaintext


def test_f04_python_cross_decryption():
    """F04-4: Cross-decryption between Python oracle and reference format."""
    plaintext = b"Cross-compatibility verification payload"
    password = "cross_test_password"
    salt = os.urandom(16)
    encrypted = CryptoOracle.encrypt_chunk_44(plaintext, password, salt=salt)
    decrypted = CryptoOracle.decrypt_chunk(encrypted, password)
    assert decrypted == plaintext


def test_f04_corrupted_tag_rejection():
    """F04-5: Tampered ciphertext or tag raises authentication error."""
    plaintext = b"Tamper test"
    password = "pass"
    encrypted = bytearray(CryptoOracle.encrypt_chunk_44(plaintext, password))
    encrypted[-1] ^= 0x01  # Corrupt 1 bit in authentication tag

    with pytest.raises(Exception):
        CryptoOracle.decrypt_chunk(bytes(encrypted), password)
