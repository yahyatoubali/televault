"""Tests for TeleVault preview engine."""

import struct

import pytest

from televault.preview import (
    PREVIEW_SIZE_SMALL,
    PREVIEW_SIZE_MEDIUM,
    PREVIEW_SIZE_LARGE,
    FilePreview,
    classify_file,
    extract_audio_metadata,
    extract_image_metadata,
    extract_video_metadata,
    generate_hex_dump,
    generate_text_preview,
)


class TestClassifyFile:
    def test_image_jpg(self):
        assert classify_file("photo.jpg") == "image"

    def test_image_png(self):
        assert classify_file("screenshot.png") == "image"

    def test_image_gif(self):
        assert classify_file("anim.gif") == "image"

    def test_image_webp(self):
        assert classify_file("photo.webp") == "image"

    def test_video_mp4(self):
        assert classify_file("movie.mp4") == "video"

    def test_video_mkv(self):
        assert classify_file("video.mkv") == "video"

    def test_audio_mp3(self):
        assert classify_file("song.mp3") == "audio"

    def test_audio_flac(self):
        assert classify_file("lossless.flac") == "audio"

    def test_text_py(self):
        assert classify_file("script.py") == "text"

    def test_text_json(self):
        assert classify_file("data.json") == "text"

    def test_text_md(self):
        assert classify_file("readme.md") == "text"

    def test_document_pdf(self):
        assert classify_file("report.pdf") == "document"

    def test_archive_zip(self):
        assert classify_file("backup.zip") == "archive"

    def test_binary_no_ext(self):
        assert classify_file("data") == "binary"

    def test_binary_unknown_ext(self):
        assert classify_file("file.xyz") == "binary"

    def test_case_insensitive(self):
        assert classify_file("Photo.JPG") == "image"


class TestGenerateHexDump:
    def test_empty(self):
        assert generate_hex_dump(b"") == ""

    def test_short_data(self):
        result = generate_hex_dump(b"Hello")
        assert "48 65 6c 6c 6f" in result
        assert "Hello" in result

    def test_max_bytes(self):
        data = b"\x00" * 512
        result = generate_hex_dump(data, max_bytes=256)
        lines = result.split("\n")
        assert len(lines) == 16  # 256 / 16

    def test_offset_format(self):
        result = generate_hex_dump(b"A")
        assert result.startswith("00000000")


class TestGenerateTextPreview:
    def test_short_text(self):
        result = generate_text_preview(b"Hello world")
        assert "Hello world" in result

    def test_long_text(self):
        lines = [f"Line {i}" for i in range(100)]
        data = "\n".join(lines).encode()
        result = generate_text_preview(data, max_lines=10)
        assert "Line 0" in result
        assert "100 total lines" in result

    def test_unicode_text(self):
        result = generate_text_preview("Hello!".encode())
        assert "Hello!" in result

    def test_binary_fallback(self):
        result = generate_text_preview(b"\xff\xfe" + b"data")
        assert len(result) > 0


class TestExtractImageMetadata:
    def test_png_header(self):
        # PNG: 8-byte signature + 4-byte length + 4-byte "IHDR" + 4-byte width + 4-byte height
        data = b"\x89PNG\r\n\x1a\n"  # PNG signature (8 bytes)
        data += struct.pack(">I", 13)  # chunk length
        data += b"IHDR"  # chunk type
        data += struct.pack(">II", 1920, 1080)  # width, height
        data += b"\x00" * 20  # padding
        meta = extract_image_metadata(data)
        assert meta["format"] == "PNG"
        assert meta.get("width") == 1920
        assert meta.get("height") == 1080

    def test_gif_header(self):
        data = b"GIF89a" + struct.pack("<HH", 800, 600) + b"\x00" * 10
        meta = extract_image_metadata(data)
        assert meta["format"] == "GIF"
        assert meta.get("width") == 800
        assert meta.get("height") == 600

    def test_unknown_data(self):
        meta = extract_image_metadata(b"\x00\x00\x00\x00")
        assert meta == {}


class TestExtractVideoMetadata:
    def test_mp4_header(self):
        data = b"\x00\x00\x00\x18" + b"ftypisom"
        meta = extract_video_metadata(data)
        assert "MP4" in meta["format"]

    def test_mkv_header(self):
        data = b"\x1a\x45\xdf\xa5" + b"\x00" * 20
        meta = extract_video_metadata(data)
        assert meta["format"] == "MKV/WebM"

    def test_unknown_data(self):
        meta = extract_video_metadata(b"\x00" * 8)
        assert meta == {}


class TestExtractAudioMetadata:
    def test_mp3_id3_header(self):
        data = b"ID3" + b"\x04\x00\x00\x00\x00\x00\x00"
        meta = extract_audio_metadata(data)
        assert meta["format"] == "MP3 (ID3)"

    def test_flac_header(self):
        meta = extract_audio_metadata(b"fLaC" + b"\x00" * 20)
        assert meta["format"] == "FLAC"

    def test_wav_header(self):
        data = b"RIFF" + b"\x00" * 4 + b"WAVE" + b"\x00" * 16
        data += struct.pack("<HHIIHH", 1, 2, 44100, 176400, 4, 16)
        meta = extract_audio_metadata(data)
        assert meta["format"] == "WAV"


class TestFilePreview:
    def test_to_dict(self):
        preview = FilePreview(
            file_id="abc123",
            name="test.png",
            size=1024,
            file_type="image",
            preview_text="test",
            metadata={"format": "PNG"},
        )
        d = preview.to_dict()
        assert d["file_id"] == "abc123"
        assert d["name"] == "test.png"
        assert d["file_type"] == "image"
        assert d["metadata"]["format"] == "PNG"
        assert "preview_text" in d

    def test_to_dict_no_preview(self):
        preview = FilePreview(file_id="abc", name="data.bin", size=2048, file_type="binary")
        d = preview.to_dict()
        assert "preview_text" not in d

    def test_preview_sizes(self):
        assert PREVIEW_SIZE_SMALL == 2048
        assert PREVIEW_SIZE_MEDIUM == 65536
        assert PREVIEW_SIZE_LARGE == 524288
