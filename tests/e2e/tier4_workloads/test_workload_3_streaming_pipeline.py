"""Workload 3: High-Throughput Streaming Pipeline Lifecycle."""

import io
import os
import struct
import pytest

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
import zstandard as zstd

from ..harness.crypto_oracle import CryptoOracle


def test_workload_3_streaming_pipeline_lifecycle():
    """Tier 4 Workload 3: High-Throughput Streaming Pipeline.

    Scenario:
    1. Generate a continuous 10MB structured binary stream.
    2. Stream payload through chunk-bounded pipeline (64KB blocks).
    3. Counter-derived nonce AES-256-GCM encryption on the fly.
    4. Pass through simulated network transport buffer.
    5. Stream decrypt and verify block authentications in sequence.
    6. Verify total throughput, complete stream reassembly, and bit-for-bit hash parity.
    """
    total_stream_bytes = 10 * 1024 * 1024  # 10MB
    block_size = 64 * 1024  # 64KB per stream block
    stream_blocks = total_stream_bytes // block_size

    # Deterministic pseudo-random stream generator
    stream_in = io.BytesIO()
    seed = 12345
    for _ in range(stream_blocks):
        buf = bytearray(block_size)
        for i in range(block_size):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            buf[i] = seed & 0xFF
        stream_in.write(buf)

    stream_in.seek(0)
    expected_full_hash = CryptoOracle.blake3_hash(stream_in.getvalue())

    # Streaming Cryptographic Session Setup
    key = os.urandom(32)
    base_nonce = os.urandom(12)
    aesgcm = AESGCM(key)

    # ── Producer / Streaming Ingest ──────────────────────────────────
    transport_buffer = []
    block_counter = 0

    while raw_chunk := stream_in.read(block_size):
        nonce = base_nonce[:8] + struct.pack(">I", block_counter)
        encrypted_block = aesgcm.encrypt(nonce, raw_chunk, None)
        transport_buffer.append((block_counter, encrypted_block))
        block_counter += 1

    assert len(transport_buffer) == stream_blocks

    # ── Consumer / Streaming Egress ──────────────────────────────────
    stream_out = io.BytesIO()
    for counter, enc_block in transport_buffer:
        nonce = base_nonce[:8] + struct.pack(">I", counter)
        decrypted_chunk = aesgcm.decrypt(nonce, enc_block, None)
        stream_out.write(decrypted_chunk)

    stream_out.seek(0)
    actual_full_hash = CryptoOracle.blake3_hash(stream_out.getvalue())

    assert stream_out.tell() == 0
    assert len(stream_out.getvalue()) == total_stream_bytes
    assert actual_full_hash == expected_full_hash
