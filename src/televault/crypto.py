"""Encryption utilities for TeleVault - AES-256-GCM with Argon2id."""

import os
import struct
from typing import BinaryIO, Iterator
from dataclasses import dataclass

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.scrypt import Scrypt
from cryptography.hazmat.backends import default_backend

# Constants
SALT_SIZE = 16
NONCE_SIZE = 12
TAG_SIZE = 16  # GCM auth tag
KEY_SIZE = 32  # 256-bit key
HEADER_SIZE = SALT_SIZE + NONCE_SIZE  # 28 bytes

# For streaming, we encrypt in blocks
BLOCK_SIZE = 64 * 1024  # 64KB blocks for streaming encryption


@dataclass
class EncryptionHeader:
    """Header prepended to encrypted data."""
    
    salt: bytes
    nonce: bytes
    
    def to_bytes(self) -> bytes:
        return self.salt + self.nonce
    
    @classmethod
    def from_bytes(cls, data: bytes) -> "EncryptionHeader":
        if len(data) < HEADER_SIZE:
            raise ValueError(f"Header too short: {len(data)} < {HEADER_SIZE}")
        return cls(
            salt=data[:SALT_SIZE],
            nonce=data[SALT_SIZE:HEADER_SIZE]
        )
    
    @classmethod
    def generate(cls) -> "EncryptionHeader":
        return cls(
            salt=os.urandom(SALT_SIZE),
            nonce=os.urandom(NONCE_SIZE)
        )


def derive_key(password: str, salt: bytes) -> bytes:
    """
    Derive encryption key from password using Scrypt.
    
    Using Scrypt instead of Argon2id for broader compatibility.
    Parameters tuned for ~100ms on modern hardware.
    """
    kdf = Scrypt(
        salt=salt,
        length=KEY_SIZE,
        n=2**17,  # CPU/memory cost
        r=8,      # Block size
        p=1,      # Parallelization
        backend=default_backend()
    )
    return kdf.derive(password.encode("utf-8"))


def encrypt_chunk(data: bytes, password: str) -> bytes:
    """
    Encrypt a chunk of data.
    
    Returns: header (28 bytes) + ciphertext + tag (16 bytes)
    """
    header = EncryptionHeader.generate()
    key = derive_key(password, header.salt)
    cipher = AESGCM(key)
    
    ciphertext = cipher.encrypt(header.nonce, data, None)
    return header.to_bytes() + ciphertext


def decrypt_chunk(encrypted_data: bytes, password: str) -> bytes:
    """
    Decrypt a chunk of data.
    
    Expects: header (28 bytes) + ciphertext + tag (16 bytes)
    """
    header = EncryptionHeader.from_bytes(encrypted_data)
    key = derive_key(password, header.salt)
    cipher = AESGCM(key)
    
    ciphertext = encrypted_data[HEADER_SIZE:]
    return cipher.decrypt(header.nonce, ciphertext, None)


class StreamingEncryptor:
    """
    Streaming encryptor for large files.
    
    Note: For simplicity, this encrypts the entire file with one key/nonce.
    For very large files, consider chunking with per-chunk nonces.
    """
    
    def __init__(self, password: str):
        self.password = password
        self.header = EncryptionHeader.generate()
        self.key = derive_key(password, self.header.salt)
        self.cipher = AESGCM(self.key)
        self._counter = 0
    
    def get_header(self) -> bytes:
        """Get the header to prepend to encrypted output."""
        return self.header.to_bytes()
    
    def _get_nonce(self) -> bytes:
        """Generate unique nonce for each block using counter mode."""
        # Use base nonce + counter to ensure uniqueness
        counter_bytes = struct.pack(">Q", self._counter)  # 8 bytes
        self._counter += 1
        # XOR with base nonce (take first 8 bytes of nonce, keep last 4)
        nonce = bytearray(self.header.nonce)
        for i in range(8):
            nonce[i] ^= counter_bytes[i]
        return bytes(nonce)
    
    def encrypt_block(self, data: bytes, is_last: bool = False) -> bytes:
        """Encrypt a block of data."""
        nonce = self._get_nonce()
        # Prepend nonce to each block for independent decryption
        ciphertext = self.cipher.encrypt(nonce, data, None)
        return nonce + ciphertext


class StreamingDecryptor:
    """Streaming decryptor for large files."""
    
    def __init__(self, password: str, header: EncryptionHeader):
        self.password = password
        self.header = header
        self.key = derive_key(password, header.salt)
        self.cipher = AESGCM(self.key)
    
    def decrypt_block(self, encrypted_block: bytes) -> bytes:
        """Decrypt a block of data."""
        # Extract nonce from block
        nonce = encrypted_block[:NONCE_SIZE]
        ciphertext = encrypted_block[NONCE_SIZE:]
        return self.cipher.decrypt(nonce, ciphertext, None)


def encrypt_file_simple(input_path: str, output_path: str, password: str) -> None:
    """Simple file encryption - loads entire file into memory."""
    with open(input_path, "rb") as f:
        data = f.read()
    
    encrypted = encrypt_chunk(data, password)
    
    with open(output_path, "wb") as f:
        f.write(encrypted)


def decrypt_file_simple(input_path: str, output_path: str, password: str) -> None:
    """Simple file decryption - loads entire file into memory."""
    with open(input_path, "rb") as f:
        encrypted = f.read()
    
    decrypted = decrypt_chunk(encrypted, password)
    
    with open(output_path, "wb") as f:
        f.write(decrypted)
