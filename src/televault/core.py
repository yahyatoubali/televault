"""Core TeleVault operations - upload, download, list."""

import asyncio
import hashlib
import os
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from .chunker import ChunkWriter, hash_data, hash_file, iter_chunks
from .compress import compress_data, decompress_data, should_compress
from .config import Config
from .crypto import decrypt_chunk, encrypt_chunk
from .models import ChunkInfo, FileMetadata
from .telegram import TelegramConfig, TelegramVault


def generate_file_id(name: str, size: int) -> str:
    """Generate short unique file ID."""
    data = f"{name}:{size}:{os.urandom(8).hex()}"
    return hashlib.sha256(data.encode()).hexdigest()[:12]


@dataclass
class UploadProgress:
    """Progress information for upload."""

    file_name: str
    total_size: int
    uploaded_size: int
    total_chunks: int
    uploaded_chunks: int
    current_chunk: int

    @property
    def percent(self) -> float:
        if self.total_chunks == 0:
            return 100.0
        return (self.uploaded_chunks / self.total_chunks) * 100


@dataclass
class DownloadProgress:
    """Progress information for download."""

    file_name: str
    total_size: int
    downloaded_size: int
    total_chunks: int
    downloaded_chunks: int
    current_chunk: int

    @property
    def percent(self) -> float:
        if self.total_chunks == 0:
            return 100.0
        return (self.downloaded_chunks / self.total_chunks) * 100


ProgressCallback = Callable[[UploadProgress | DownloadProgress], None]


class TeleVault:
    """
    Main TeleVault interface.

    Handles file upload, download, listing with compression and encryption.
    """

    def __init__(
        self,
        config: Config | None = None,
        telegram_config: TelegramConfig | None = None,
        password: str | None = None,
    ):
        self.config = config or Config.load_or_create()
        self.telegram = TelegramVault(telegram_config)
        self.password = password
        self._connected = False

    async def is_authenticated(self) -> bool:
        """Check if user is authenticated with Telegram."""
        return await self.telegram._client.is_user_authorized()

    async def connect(self, skip_channel: bool = False) -> None:
        """Connect to Telegram."""
        await self.telegram.connect()

        if not skip_channel and self.config.channel_id and await self.is_authenticated():
            await self.telegram.set_channel(self.config.channel_id)

        self._connected = True

    async def disconnect(self) -> None:
        """Disconnect from Telegram."""
        await self.telegram.disconnect()
        self._connected = False

    async def login(self, phone: str | None = None) -> str:
        """Interactive login flow."""
        return await self.telegram.login(phone)

    async def setup_channel(self, channel_id: int | None = None) -> int:
        """Set up storage channel."""
        if channel_id:
            await self.telegram.set_channel(channel_id)
            self.config.channel_id = channel_id
        else:
            channel_id = await self.telegram.create_channel()
            self.config.channel_id = channel_id

        self.config.save()
        return channel_id

    async def upload(
        self,
        file_path: str | Path,
        password: str | None = None,
        progress_callback: ProgressCallback | None = None,
        preserve_path: bool = False,
    ) -> FileMetadata:
        """
        Upload a file to TeleVault with parallel chunk uploads.

        Args:
            file_path: Path to file to upload
            password: Encryption password (uses instance password if not provided)
            progress_callback: Optional progress callback
            preserve_path: If True, include full path in filename (for directory uploads)

        Returns:
            FileMetadata of uploaded file
        """
        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        file_path = Path(file_path)
        if not file_path.exists():
            raise FileNotFoundError(f"File not found: {file_path}")

        password = password or self.password

        # Get file info
        file_name = file_path.name
        if preserve_path:
            # Use full path relative to upload root (replace / with _ for safety)
            # For now, just use the full path
            file_name = str(file_path)
            file_name = file_name.replace("/", "_")

        file_size = file_path.stat().st_size
        file_hash = hash_file(file_path)
        file_id = generate_file_id(file_name, file_size)

        # Count chunks
        chunk_size = self.config.chunk_size
        total_chunks = (file_size + chunk_size - 1) // chunk_size
        if total_chunks == 0:
            total_chunks = 1  # Empty file = 1 empty chunk

        # Create initial metadata
        metadata = FileMetadata(
            id=file_id,
            name=file_name,
            size=file_size,
            hash=file_hash,
            encrypted=self.config.encryption and password is not None,
            compressed=self.config.compression and should_compress(file_name),
        )

        # Upload metadata message first
        metadata_msg_id = await self.telegram.upload_metadata(metadata)
        metadata.message_id = metadata_msg_id

        # Prepare chunks for parallel upload
        chunk_results: dict[int, ChunkInfo] = {}
        uploaded_count = 0
        lock = asyncio.Lock()

        async def upload_single_chunk(chunk):
            nonlocal uploaded_count

            data = chunk.data

            # Compress if enabled
            if metadata.compressed:
                data = compress_data(data)

            # Encrypt if enabled
            if metadata.encrypted and password:
                data = encrypt_chunk(data, password)

            # Upload chunk
            chunk_msg_id = await self.telegram.upload_chunk(
                data=data,
                filename=f"{file_id}_{chunk.index:04d}.chunk",
                reply_to=metadata_msg_id,
            )

            # Track chunk info
            chunk_info = ChunkInfo(
                index=chunk.index,
                message_id=chunk_msg_id,
                size=len(data),
                hash=hash_data(data),
            )

            async with lock:
                chunk_results[chunk.index] = chunk_info
                uploaded_count += 1

                # Progress callback
                if progress_callback:
                    progress_callback(
                        UploadProgress(
                            file_name=file_name,
                            total_size=file_size,
                            uploaded_size=int(file_size * uploaded_count / total_chunks),
                            total_chunks=total_chunks,
                            uploaded_chunks=uploaded_count,
                            current_chunk=chunk.index,
                        )
                    )

        # Upload chunks in parallel (limited concurrency)
        semaphore = asyncio.Semaphore(self.config.parallel_uploads)

        async def upload_with_limit(chunk):
            async with semaphore:
                await upload_single_chunk(chunk)

        # Collect all chunks first for parallel processing
        chunks = list(iter_chunks(file_path, chunk_size))

        if chunks:
            await asyncio.gather(*[upload_with_limit(c) for c in chunks])

        # Sort chunks by index
        metadata.chunks = [chunk_results[i] for i in sorted(chunk_results.keys())]

        # Update metadata with chunk info
        await self.telegram.update_metadata(metadata_msg_id, metadata)

        # Update index
        index = await self.telegram.get_index()
        index.add_file(file_id, metadata_msg_id)
        await self.telegram.save_index(index)

        return metadata

    async def download(
        self,
        file_id_or_name: str,
        output_path: str | Path | None = None,
        password: str | None = None,
        progress_callback: ProgressCallback | None = None,
    ) -> Path:
        """
        Download a file from TeleVault.

        Args:
            file_id_or_name: File ID or name to download
            output_path: Output path (uses original filename in current dir if not provided)
            password: Decryption password
            progress_callback: Optional progress callback

        Returns:
            Path to downloaded file
        """
        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        password = password or self.password

        # Find file
        index = await self.telegram.get_index()

        # Try as file ID first
        if file_id_or_name in index.files:
            metadata_msg_id = index.files[file_id_or_name]
        else:
            # Search by name
            files = await self.telegram.list_files()
            matches = [f for f in files if f.name == file_id_or_name or file_id_or_name in f.name]

            if not matches:
                raise FileNotFoundError(f"File not found: {file_id_or_name}")
            if len(matches) > 1:
                raise ValueError(
                    f"Multiple files match '{file_id_or_name}': {[f.name for f in matches]}"
                )

            metadata_msg_id = matches[0].message_id

        # Get metadata
        metadata = await self.telegram.get_metadata(metadata_msg_id)

        # Determine output path
        output_path = Path(output_path) if output_path else Path.cwd() / metadata.name

        # Create chunk writer
        writer = ChunkWriter(output_path, metadata.size, self.config.chunk_size)

        downloaded_size = 0
        total_chunks = len(metadata.chunks)

        # Download chunks in order
        for downloaded_chunks, chunk_info in enumerate(
            sorted(metadata.chunks, key=lambda c: c.index), start=1
        ):
            # Download chunk
            data = await self.telegram.download_chunk(chunk_info.message_id)

            # Verify hash
            if hash_data(data) != chunk_info.hash:
                raise ValueError(f"Chunk {chunk_info.index} hash mismatch - data corrupted")

            # Decrypt if needed
            if metadata.encrypted:
                if not password:
                    raise ValueError("File is encrypted but no password provided")
                data = decrypt_chunk(data, password)

            # Decompress if needed
            if metadata.compressed:
                data = decompress_data(data)

            # Write chunk
            from .chunker import Chunk

            writer.write_chunk(
                Chunk(
                    index=chunk_info.index,
                    data=data,
                    hash="",  # Already verified
                    size=len(data),
                )
            )

            downloaded_size += len(data)

            # Progress callback
            if progress_callback:
                progress_callback(
                    DownloadProgress(
                        file_name=metadata.name,
                        total_size=metadata.size,
                        downloaded_size=downloaded_size,
                        total_chunks=total_chunks,
                        downloaded_chunks=downloaded_chunks,
                        current_chunk=chunk_info.index,
                    )
                )

        # Verify final hash
        if hash_file(output_path) != metadata.hash:
            output_path.unlink(missing_ok=True)  # Delete corrupted file if it exists
            raise ValueError(
                "Downloaded file hash mismatch - downloaded data is corrupted; "
                "try re-downloading or checking your network/Telegram storage."
            )

        return output_path

    async def list_files(self) -> list[FileMetadata]:
        """List all files in the vault."""
        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        return await self.telegram.list_files()

    async def search(self, query: str) -> list[FileMetadata]:
        """Search files by name."""
        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        return await self.telegram.search_files(query)

    async def delete(self, file_id_or_name: str) -> bool:
        """Delete a file."""
        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        index = await self.telegram.get_index()

        # Try as file ID first
        if file_id_or_name in index.files:
            return await self.telegram.delete_file(file_id_or_name)

        # Search by name
        files = await self.telegram.list_files()
        matches = [f for f in files if f.name == file_id_or_name]

        if not matches:
            return False
        if len(matches) > 1:
            raise ValueError(f"Multiple files match '{file_id_or_name}'")

        return await self.telegram.delete_file(matches[0].id)

    async def get_status(self) -> dict:
        """Get vault status."""
        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        files = await self.list_files()
        total_size = sum(f.size for f in files)
        stored_size = sum(f.total_stored_size for f in files)

        return {
            "channel_id": self.config.channel_id,
            "file_count": len(files),
            "total_size": total_size,
            "stored_size": stored_size,
            "compression_ratio": stored_size / total_size if total_size > 0 else 1.0,
        }
