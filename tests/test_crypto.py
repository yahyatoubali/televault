"""Tests for TeleVault crypto module."""

import os
import pytest

from televault.crypto import (
    encrypt_chunk,
    decrypt_chunk,
    derive_key,
    EncryptionHeader,
    HEADER_SIZE,
)


def test_derive_key_deterministic():
    """Key derivation should be deterministic with same salt."""
    password = "test_password"
    salt = b"x" * 16
    
    key1 = derive_key(password, salt)
    key2 = derive_key(password, salt)
    
    assert key1 == key2
    assert len(key1) == 32


def test_derive_key_different_salts():
    """Different salts should produce different keys."""
    password = "test_password"
    
    key1 = derive_key(password, b"a" * 16)
    key2 = derive_key(password, b"b" * 16)
    
    assert key1 != key2


def test_encryption_header():
    """Test encryption header generation and parsing."""
    header = EncryptionHeader.generate()
    
    assert len(header.salt) == 16
    assert len(header.nonce) == 12
    
    serialized = header.to_bytes()
    assert len(serialized) == HEADER_SIZE
    
    parsed = EncryptionHeader.from_bytes(serialized)
    assert parsed.salt == header.salt
    assert parsed.nonce == header.nonce


def test_encrypt_decrypt_roundtrip():
    """Test encryption/decryption roundtrip."""
    password = "my_secret_password"
    plaintext = b"Hello, World! This is a test message."
    
    encrypted = encrypt_chunk(plaintext, password)
    
    # Encrypted should be larger (header + tag)
    assert len(encrypted) > len(plaintext)
    
    decrypted = decrypt_chunk(encrypted, password)
    assert decrypted == plaintext


def test_encrypt_decrypt_large_data():
    """Test encryption of larger data."""
    password = "password123"
    plaintext = os.urandom(1024 * 1024)  # 1MB
    
    encrypted = encrypt_chunk(plaintext, password)
    decrypted = decrypt_chunk(encrypted, password)
    
    assert decrypted == plaintext


def test_decrypt_wrong_password():
    """Decryption with wrong password should fail."""
    plaintext = b"secret data"
    
    encrypted = encrypt_chunk(plaintext, "correct_password")
    
    with pytest.raises(Exception):  # cryptography raises InvalidTag
        decrypt_chunk(encrypted, "wrong_password")


def test_encrypt_empty_data():
    """Test encryption of empty data."""
    password = "password"
    plaintext = b""
    
    encrypted = encrypt_chunk(plaintext, password)
    decrypted = decrypt_chunk(encrypted, password)
    
    assert decrypted == plaintext


def test_encrypt_different_each_time():
    """Same plaintext should encrypt differently each time (random nonce/salt)."""
    password = "password"
    plaintext = b"same data"
    
    encrypted1 = encrypt_chunk(plaintext, password)
    encrypted2 = encrypt_chunk(plaintext, password)
    
    # Should be different due to random salt/nonce
    assert encrypted1 != encrypted2
    
    # But both should decrypt to same plaintext
    assert decrypt_chunk(encrypted1, password) == plaintext
    assert decrypt_chunk(encrypted2, password) == plaintext
