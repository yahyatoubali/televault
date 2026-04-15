"""Preview engine for TeleVault - generate terminal previews without full download."""

import logging
import struct
from dataclasses import dataclass, field
from pathlib import Path

from .compress import decompress_data
from .config import Config
from .core import TeleVault
from .crypto import decrypt_chunk
from .models import FileMetadata
from .telegram import TelegramConfig
from .utils import format_size as _fmt_size

logger = logging.getLogger("televault.preview")

IMAGE_EXTENSIONS = {
    ".jpg",
    ".jpeg",
    ".png",
    ".gif",
    ".webp",
    ".bmp",
    ".ico",
    ".tiff",
    ".tif",
    ".heic",
    ".heif",
    ".avif",
}
VIDEO_EXTENSIONS = {
    ".mp4",
    ".mkv",
    ".avi",
    ".mov",
    ".webm",
    ".m4v",
    ".wmv",
    ".flv",
    ".mpg",
    ".mpeg",
    ".3gp",
}
AUDIO_EXTENSIONS = {
    ".mp3",
    ".wav",
    ".ogg",
    ".flac",
    ".aac",
    ".m4a",
    ".opus",
    ".wma",
    ".aiff",
}
TEXT_EXTENSIONS = {
    ".txt",
    ".md",
    ".py",
    ".js",
    ".ts",
    ".go",
    ".rs",
    ".c",
    ".cpp",
    ".h",
    ".java",
    ".rb",
    ".php",
    ".html",
    ".css",
    ".json",
    ".xml",
    ".yaml",
    ".yml",
    ".toml",
    ".ini",
    ".cfg",
    ".conf",
    ".sh",
    ".bash",
    ".zsh",
    ".fish",
    ".ps1",
    ".bat",
    ".sql",
    ".r",
    ".csv",
    ".log",
    ".env",
}
DOCUMENT_EXTENSIONS = {
    ".pdf",
    ".doc",
    ".docx",
    ".xls",
    ".xlsx",
    ".ppt",
    ".pptx",
    ".odt",
    ".ods",
    ".odp",
}
ARCHIVE_EXTENSIONS = {
    ".zip",
    ".tar",
    ".gz",
    ".bz2",
    ".xz",
    ".7z",
    ".rar",
    ".zst",
}

PREVIEW_SIZE_SMALL = 2048
PREVIEW_SIZE_MEDIUM = 65536
PREVIEW_SIZE_LARGE = 524288


@dataclass
class FilePreview:
    """Preview result for a file."""

    file_id: str
    name: str
    size: int
    file_type: str  # "image", "video", "audio", "text", "document", "archive", "binary"
    preview_text: str = ""
    metadata: dict = field(default_factory=dict)
    hex_dump: str = ""
    first_bytes: bytes = b""

    def to_dict(self) -> dict:
        d = {
            "file_id": self.file_id,
            "name": self.name,
            "size": self.size,
            "file_type": self.file_type,
            "metadata": self.metadata,
        }
        if self.preview_text:
            d["preview_text"] = self.preview_text
        if self.hex_dump:
            d["hex_dump"] = self.hex_dump
        return d


def classify_file(filename: str) -> str:
    """Classify a file by its extension."""
    ext = Path(filename).suffix.lower()
    if ext in IMAGE_EXTENSIONS:
        return "image"
    if ext in VIDEO_EXTENSIONS:
        return "video"
    if ext in AUDIO_EXTENSIONS:
        return "audio"
    if ext in TEXT_EXTENSIONS:
        return "text"
    if ext in DOCUMENT_EXTENSIONS:
        return "document"
    if ext in ARCHIVE_EXTENSIONS:
        return "archive"
    return "binary"


def generate_hex_dump(data: bytes, max_bytes: int = 256) -> str:
    """Generate a hex dump of the first N bytes."""
    lines = []
    data = data[:max_bytes]
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{offset:08x}  {hex_part:<48s}  {ascii_part}")
    return "\n".join(lines)


def generate_text_preview(data: bytes, max_lines: int = 40) -> str:
    """Generate a text preview from raw bytes."""
    try:
        text = data.decode("utf-8", errors="replace")
    except Exception:
        text = data.decode("latin-1", errors="replace")

    lines = text.splitlines()
    if len(lines) > max_lines:
        return "\n".join(lines[:max_lines]) + f"\n... ({len(lines)} total lines)"
    return "\n".join(lines)


def extract_image_metadata(data: bytes) -> dict:
    """Extract metadata from image headers."""
    meta = {}

    if data[:8] == b"\x89PNG\r\n\x1a\n":
        meta["format"] = "PNG"
        if len(data) > 24:
            w = struct.unpack(">I", data[16:20])[0]
            h = struct.unpack(">I", data[20:24])[0]
            meta["width"] = w
            meta["height"] = h

    elif data[:2] == b"\xff\xd8":
        meta["format"] = "JPEG"
        i = 2
        while i < min(len(data) - 1, 65536):
            if data[i] != 0xFF:
                break
            marker = data[i + 1]
            if marker in (0xC0, 0xC1, 0xC2):
                if i + 9 <= len(data):
                    h = struct.unpack(">H", data[i + 5 : i + 7])[0]
                    w = struct.unpack(">H", data[i + 7 : i + 9])[0]
                    meta["width"] = w
                    meta["height"] = h
                break
            elif marker in (0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9):
                i += 2
            elif i + 3 < len(data):
                length = struct.unpack(">H", data[i + 2 : i + 4])[0]
                i += 2 + length
            else:
                break

    elif data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        meta["format"] = "WebP"

    elif data[:6] in (b"GIF87a", b"GIF89a"):
        meta["format"] = "GIF"
        if len(data) > 10:
            w = struct.unpack("<H", data[6:8])[0]
            h = struct.unpack("<H", data[8:10])[0]
            meta["width"] = w
            meta["height"] = h

    elif data[:4] == b"BM":
        meta["format"] = "BMP"

    return meta


def extract_video_metadata(data: bytes) -> dict:
    """Extract metadata from video file headers."""
    meta = {}

    if data[:4] == b"\x1a\x45\xdf\xa5":
        meta["format"] = "MKV/WebM"
    elif data[4:8] == b"ftyp":
        brand = data[8:12].decode("ascii", errors="replace").strip()
        meta["format"] = f"MP4 ({brand})"
    elif data[:4] == b"RIFF" and len(data) > 10 and data[8:12] == b"AVI ":
        meta["format"] = "AVI"

    return meta


def extract_audio_metadata(data: bytes) -> dict:
    """Extract metadata from audio file headers."""
    meta = {}

    if data[:3] == b"ID3":
        meta["format"] = "MP3 (ID3)"
        try:
            size_flags = data[6:10]
            synsafe = 0
            for b in size_flags:
                synsafe = synsafe * 128 + b
            id3_end = 10 + synsafe
            meta["id3_size"] = id3_end
        except Exception:
            pass

    elif data[:4] == b"fLaC":
        meta["format"] = "FLAC"

    elif data[:4] == b"OggS":
        meta["format"] = "OGG"

    elif data[:4] == b"RIFF" and data[8:12] == b"WAVE":
        meta["format"] = "WAV"
        if len(data) > 28:
            channels = struct.unpack("<H", data[22:24])[0]
            sample_rate = struct.unpack("<I", data[24:28])[0]
            meta["channels"] = channels
            meta["sample_rate"] = sample_rate

    return meta


def render_ascii_image(width: int, height: int, format_name: str, size: int) -> str:
    """Render an ASCII representation of an image file."""
    aspect = width / height if height > 0 else 1
    cols = min(40, max(20, int(aspect * 10)))
    rows = max(5, min(12, int(cols / aspect / 2)))

    block = "\u2591"
    lines = []
    lines.append(f"  +{'─' * cols}+")
    for _ in range(rows):
        lines.append(f"  |{block * cols}|")
    lines.append(f"  +{'─' * cols}+")

    dim_line = f"  {width}x{height} {format_name}" if width and height else f"  {format_name}"
    lines.insert(1, dim_line)

    return "\n".join(lines)


class PreviewEngine:
    """Generate terminal previews for vault files without full download."""

    def __init__(
        self,
        config: Config | None = None,
        telegram_config: TelegramConfig | None = None,
        password: str | None = None,
    ):
        self.config = config or Config.load_or_create()
        self.password = password
        self._telegram_config = telegram_config
        self._vault: TeleVault | None = None

    async def _ensure_connected(self):
        if self._vault is None:
            self._vault = TeleVault(
                config=self.config,
                telegram_config=self._telegram_config,
                password=self.password,
            )
            await self._vault.connect()

    async def disconnect(self):
        if self._vault:
            await self._vault.disconnect()
            self._vault = None

    async def preview(
        self,
        file_id_or_name: str,
        size: str = "small",
        password: str | None = None,
    ) -> FilePreview:
        """Generate a preview for a file in the vault."""
        await self._ensure_connected()
        password = password or self.password

        index = await self._vault.telegram.get_index()

        metadata_msg_id = None
        if file_id_or_name in index.files:
            metadata_msg_id = index.files[file_id_or_name]
        else:
            files = await self._vault.list_files()
            matches = [f for f in files if f.name == file_id_or_name or file_id_or_name in f.name]
            if not matches:
                raise FileNotFoundError(f"File not found: {file_id_or_name}")
            if len(matches) > 1:
                raise ValueError(f"Multiple files match '{file_id_or_name}'")
            metadata_msg_id = matches[0].message_id

        metadata = await self._vault.telegram.get_metadata(metadata_msg_id)
        file_type = classify_file(metadata.name)

        size_bytes = {
            "small": PREVIEW_SIZE_SMALL,
            "medium": PREVIEW_SIZE_MEDIUM,
            "large": PREVIEW_SIZE_LARGE,
        }.get(size, PREVIEW_SIZE_SMALL)

        first_bytes = await self._download_first_bytes(metadata, size_bytes, password)

        preview_text = ""
        file_metadata = {}

        if file_type == "image":
            file_metadata = extract_image_metadata(first_bytes)
            w = file_metadata.get("width", 0)
            h = file_metadata.get("height", 0)
            if w and h:
                file_metadata["dimensions"] = f"{w}x{h}"
            file_metadata["file_size"] = _fmt_size(metadata.size)
            if metadata.compressed:
                file_metadata["compression"] = "Yes"
            if metadata.encrypted:
                file_metadata["encryption"] = "Yes"
            preview_text = ""

        elif file_type == "video":
            file_metadata = extract_video_metadata(first_bytes)
            file_metadata["file_size"] = _fmt_size(metadata.size)
            if metadata.compressed:
                file_metadata["compression"] = "Yes"
            if metadata.encrypted:
                file_metadata["encryption"] = "Yes"
            preview_text = ""

        elif file_type == "audio":
            file_metadata = extract_audio_metadata(first_bytes)
            file_metadata["file_size"] = _fmt_size(metadata.size)
            if metadata.compressed:
                file_metadata["compression"] = "Yes"
            if metadata.encrypted:
                file_metadata["encryption"] = "Yes"
            preview_text = ""

        elif file_type == "text":
            preview_text = generate_text_preview(first_bytes, max_lines=40)

        else:
            hex_dump = generate_hex_dump(first_bytes)
            preview_text = hex_dump

        return FilePreview(
            file_id=metadata.id,
            name=metadata.name,
            size=metadata.size,
            file_type=file_type,
            preview_text=preview_text,
            metadata=file_metadata,
            hex_dump=generate_hex_dump(first_bytes) if file_type not in ("text",) else "",
            first_bytes=first_bytes[:256],
        )

    async def _download_first_bytes(
        self, metadata: FileMetadata, max_bytes: int, password: str | None
    ) -> bytes:
        """Download just enough chunks to get the first max_bytes."""
        if not metadata.chunks:
            return b""

        sorted_chunks = sorted(metadata.chunks, key=lambda c: c.index)
        first_chunk = sorted_chunks[0]

        data = await self._vault.telegram.download_chunk(first_chunk.message_id)

        if metadata.encrypted and password:
            data = decrypt_chunk(data, password)

        if metadata.compressed:
            data = decompress_data(data)

        return data[:max_bytes]
