"""Tests for Telegram helper functions (compression)."""

import pytest

from televault.telegram import TELEGRAM_MSG_LIMIT, TV_PREFIX, _compress_message, _decompress_message


class TestCompressMessage:
    def test_small_message_not_compressed(self):
        text = '{"version":1,"files":{}}'
        result = _compress_message(text)
        assert result == text
        assert not result.startswith(TV_PREFIX)

    def test_large_message_compressed(self):
        text = (
            '{"version":1,"files":{'
            + ",".join(f'"long_file_name_{i:04d}":{100000 + i}' for i in range(300))
            + "}}"
        )
        assert len(text) > TELEGRAM_MSG_LIMIT
        result = _compress_message(text)
        assert result.startswith(TV_PREFIX)
        assert len(result) < len(text)

    def test_compress_decompress_roundtrip_small(self):
        text = '{"version":1,"files":{}}'
        assert _decompress_message(_compress_message(text)) == text

    def test_compress_decompress_roundtrip_large(self):
        text = '{"version":1,"files":{' + ",".join(f'"file_{i:04d}":{i}' for i in range(200)) + "}}"
        compressed = _compress_message(text)
        assert _decompress_message(compressed) == text

    def test_decompress_plain_text(self):
        text = "not compressed"
        assert _decompress_message(text) == text

    def test_compressed_fits_limit(self):
        text = '{"version":1,"files":{' + ",".join(f'"file_{i:04d}":{i}' for i in range(200)) + "}}"
        result = _compress_message(text)
        assert len(result) <= TELEGRAM_MSG_LIMIT

    def test_file_metadata_large_file(self):
        from televault.models import ChunkInfo, FileMetadata

        chunks = [
            ChunkInfo(
                index=i,
                message_id=1000 + i,
                size=104857600,
                hash="a" * 64,
                original_hash="b" * 64,
            )
            for i in range(50)
        ]
        metadata = FileMetadata(
            id="abc123def456",
            name="large_file.iso",
            size=5368709120,
            hash="c" * 64,
            chunks=chunks,
            encrypted=True,
            compressed=True,
        )
        json_text = metadata.to_json()
        assert len(json_text) > TELEGRAM_MSG_LIMIT
        compressed = _compress_message(json_text)
        assert len(compressed) <= TELEGRAM_MSG_LIMIT
        decompressed = _decompress_message(compressed)
        original = FileMetadata.from_json(decompressed)
        assert original.id == metadata.id
        assert original.name == metadata.name
        assert original.size == metadata.size
        assert len(original.chunks) == 50

    def test_vault_index_many_files(self):
        from televault.models import VaultIndex

        index = VaultIndex()
        for i in range(500):
            index.add_file(f"long_file_name_{i:04d}", 10000 + i)

        json_text = index.to_json()
        assert len(json_text) > TELEGRAM_MSG_LIMIT
        compressed = _compress_message(json_text)
        assert len(compressed) <= TELEGRAM_MSG_LIMIT
        decompressed = _decompress_message(compressed)
        original = VaultIndex.from_json(decompressed)
        assert len(original.files) == 500
