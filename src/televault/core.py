"""Core TeleVault operations - upload, download, list."""

import asyncio
import contextlib
import hashlib
import logging
import os
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from .chunker import (
    ChunkWriter,
    hash_data,
    hash_data_async,
    hash_file,
    hash_file_async,
    iter_chunks,
    iter_chunks_async,
)
from .compress import compress_data, decompress_data, should_compress
from .config import Config
from .crypto import decrypt_chunk, encrypt_chunk
from .models import (
    ChunkInfo,
    FileMetadata,
    TransferProgress,
    load_progress_with_crc,
    save_progress_with_crc,
)
from .telegram import TelegramConfig, TelegramVault

logger = logging.getLogger("televault.core")


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
    phase: str = "uploading"  # "hashing", "metadata", "uploading", "index", "done"

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
    phase: str = "downloading"  # "metadata", "downloading", "verifying", "done"

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
        
        # Apply low-resource mode settings if enabled
        if self.config.low_resource_mode:
            self._apply_low_resource_settings()
        
        self.telegram = TelegramVault(telegram_config)
        self.password = password
        self._connected = False
        self._index_lock = asyncio.Lock()

    def _apply_low_resource_settings(self) -> None:
        """Apply low-resource mode optimizations."""
        # Reduce chunk size to lower memory usage
        self.config.chunk_size = self.config.low_resource_chunk_size
        
        # Reduce parallelism to lower CPU and memory pressure
        self.config.parallel_uploads = min(
            self.config.parallel_uploads, 
            self.config.low_resource_parallelism
        )
        self.config.parallel_downloads = min(
            self.config.parallel_downloads, 
            self.config.low_resource_parallelism
        )
        
        logger.info(
            f"Low-resource mode enabled: "
            f"chunk_size={self.config.chunk_size // (1024*1024)}MB, "
            f"parallel_uploads={self.config.parallel_uploads}, "
            f"parallel_downloads={self.config.parallel_downloads}"
        )

    async def is_authenticated(self) -> bool:
        """Check if user is authenticated with Telegram."""
        if not self._connected or self.telegram._client is None:
            return False
        return await self.telegram._client.is_user_authorized()

    async def get_account_info(self) -> dict:
        """Get current account info."""
        if not self._connected or self.telegram._client is None:
            raise RuntimeError("Not connected. Call connect() first.")
        me = await self.telegram._client.get_me()
        if me is None:
            return {}
        return {
            "id": me.id,
            "first_name": me.first_name,
            "last_name": me.last_name,
            "username": me.username,
            "phone": me.phone,
        }

    async def test_channel(self, channel_id: int) -> dict:
        """Test if a channel is accessible and writable."""
        if not self._connected or self.telegram._client is None:
            raise RuntimeError("Not connected. Call connect() first.")
        return await self.telegram.test_channel(channel_id)

    async def list_channels(self) -> list[dict]:
        """List all channels the user is a member of."""
        if not self._connected or self.telegram._client is None:
            raise RuntimeError("Not connected. Call connect() first.")
        return await self.telegram.list_channels()

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
        name: str | None = None,
        if_exists: str = "version",  # "version", "replace", "skip"
    ) -> FileMetadata:
        """
        Upload a file to TeleVault with parallel chunk uploads.

        Args:
            file_path: Path to file to upload
            password: Encryption password (uses instance password if not provided)
            progress_callback: Optional progress callback
            preserve_path: If True, include full path in filename (for directory uploads)
            name: Override filename (used by upload_stream to avoid double index save)
            if_exists: Behavior when file with same name exists:
                      - "version": Append version number (default)
                      - "replace": Replace existing file
                      - "skip": Skip upload

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
        if name:
            file_name = name
        elif preserve_path:
            file_name = str(file_path).replace("/", "_")
        else:
            file_name = file_path.name

        # Check for duplicate file names
        if if_exists != "version" and not name:
            index = await self.telegram.get_index()
            files = await self.telegram.list_files()
            matches = [f for f in files if f.name == file_name]

            if matches:
                if if_exists == "skip":
                    if progress_callback:
                        progress_callback(
                            UploadProgress(
                                file_name=file_name,
                                total_size=0,
                                uploaded_size=0,
                                total_chunks=0,
                                uploaded_chunks=0,
                                current_chunk=0,
                                phase="done",
                            )
                        )
                    return matches[0]
                elif if_exists == "replace":
                    # Delete existing file first
                    await self.delete(matches[0].id)

        file_size = file_path.stat().st_size

        # Auto-version duplicate file names
        if if_exists == "version" and not name:
            original_name = file_name
            version = 1
            files = await self.telegram.list_files()
            while any(f.name == file_name for f in files):
                name_parts = original_name.rsplit(".", 1)
                if len(name_parts) == 2:
                    file_name = f"{name_parts[0]}_v{version}.{name_parts[1]}"
                else:
                    file_name = f"{original_name}_v{version}"
                version += 1

        if progress_callback:
            progress_callback(
                UploadProgress(
                    file_name=file_name if name else file_path.name,
                    total_size=file_size,
                    uploaded_size=0,
                    total_chunks=0,
                    uploaded_chunks=0,
                    current_chunk=0,
                    phase="hashing",
                )
            )

        # Use async hashing to avoid blocking the event loop
        file_hash = await hash_file_async(file_path)
        file_id = generate_file_id(file_name, file_size)

        # Count chunks
        chunk_size = self.config.chunk_size
        total_chunks = (file_size + chunk_size - 1) // chunk_size
        if total_chunks == 0:
            total_chunks = 1  # Empty file = 1 empty chunk

        # In low-resource mode, disable compression for very large files to save memory
        if self.config.low_resource_mode and file_size > 500 * 1024 * 1024:  # 500MB
            logger.info(
                f"Low-resource mode: Disabling compression for large file ({file_size} bytes) "
                "to reduce memory pressure"
            )
            metadata.compressed = False

        # Create initial metadata
        metadata = FileMetadata(
            id=file_id,
            name=file_name,
            size=file_size,
            hash=file_hash,
            encrypted=self.config.encryption and password is not None,
            compressed=self.config.compression and should_compress(file_name),
        )

        if progress_callback:
            progress_callback(
                UploadProgress(
                    file_name=file_name,
                    total_size=file_size,
                    uploaded_size=0,
                    total_chunks=total_chunks,
                    uploaded_chunks=0,
                    current_chunk=0,
                    phase="metadata",
                )
            )

        # Upload metadata message first
        metadata_msg_id = await self.telegram.upload_metadata(metadata)
        metadata.message_id = metadata_msg_id

        # Streaming parallel upload: use a queue so only N chunks are in memory at once
        chunk_results: dict[int, ChunkInfo] = {}
        uploaded_count = 0
        lock = asyncio.Lock()
        uploaded_msg_ids: list[int] = []

        semaphore = asyncio.Semaphore(self.config.parallel_uploads)
        chunk_queue: asyncio.Queue = asyncio.Queue(maxsize=self.config.parallel_uploads * 2)

        async def process_and_upload(chunk):
            nonlocal uploaded_count

            async with semaphore:
                data = chunk.data
                # Free the raw chunk data reference early
                chunk_data_raw = chunk.data
                # Use async hashing to avoid blocking
                original_hash = await hash_data_async(chunk_data_raw)

                if metadata.compressed:
                    data = compress_data(data)
                if metadata.encrypted and password:
                    data = encrypt_chunk(data, password)

                chunk_msg_id = await self.telegram.upload_chunk(
                    data=data,
                    filename=f"{file_id}_{chunk.index:04d}.chunk",
                    reply_to=metadata_msg_id,
                )

                # Hash the processed data asynchronously
                processed_hash = await hash_data_async(data)
                chunk_info = ChunkInfo(
                    index=chunk.index,
                    message_id=chunk_msg_id,
                    size=len(data),
                    hash=processed_hash,
                    original_hash=original_hash,
                )

                async with lock:
                    chunk_results[chunk.index] = chunk_info
                    uploaded_msg_ids.append(chunk_msg_id)
                    uploaded_count += 1

                    if progress_callback:
                        progress_callback(
                            UploadProgress(
                                file_name=file_name,
                                total_size=file_size,
                                uploaded_size=int(file_size * uploaded_count / total_chunks),
                                total_chunks=total_chunks,
                                uploaded_chunks=uploaded_count,
                                current_chunk=chunk.index,
                                phase="uploading",
                            )
                        )

        # Producer: read chunks from disk asynchronously and feed the queue
        async def producer():
            try:
                async for chunk in iter_chunks_async(file_path, chunk_size):
                    await chunk_queue.put(chunk)
            finally:
                # Signal all consumers done
                await chunk_queue.put(None)

        # Consumers: process chunks from the queue
        tasks = []
        try:
            prod_task = asyncio.create_task(producer())

            while True:
                chunk = await chunk_queue.get()
                if chunk is None:
                    break
                tasks.append(asyncio.create_task(process_and_upload(chunk)))

            # Wait for all uploads to finish
            if tasks:
                await asyncio.gather(*tasks)

            await prod_task
        except Exception:
            # Cleanup on failure
            logger.error(f"Upload failed for {file_name}, cleaning up...")
            all_ids = uploaded_msg_ids + [metadata_msg_id]
            with contextlib.suppress(Exception):
                await self.telegram._client.delete_messages(self.telegram._channel_id, all_ids)
            raise

        # Sort chunks by index
        metadata.chunks = [chunk_results[i] for i in sorted(chunk_results.keys())]

        # Update metadata with chunk info
        if progress_callback:
            progress_callback(
                UploadProgress(
                    file_name=file_name,
                    total_size=file_size,
                    uploaded_size=file_size,
                    total_chunks=total_chunks,
                    uploaded_chunks=total_chunks,
                    current_chunk=total_chunks - 1,
                    phase="index",
                )
            )

        await self.telegram.update_metadata(metadata_msg_id, metadata)

        async with self._index_lock:
            max_index_retries = 3
            for attempt in range(max_index_retries):
                try:
                    index = await self.telegram.get_index()
                    if file_id not in index.files:
                        index.add_file(file_id, metadata_msg_id)
                    await self.telegram.save_index(index)
                    break
                except Exception as e:
                    if attempt >= max_index_retries - 1:
                        logger.error(f"Failed to save index after upload: {e}")
                        logger.error(
                            f"File data is safe on Telegram (metadata msg {metadata_msg_id})"
                        )
                        logger.error(
                            "Run 'tvt gc --clean-partials' to clean up, or 'tvt push' again"
                        )
                        raise
                    logger.warning(f"Index save retry {attempt + 1}: {e}")
                    await asyncio.sleep(0.5 * (attempt + 1))

        if progress_callback:
            progress_callback(
                UploadProgress(
                    file_name=file_name,
                    total_size=file_size,
                    uploaded_size=file_size,
                    total_chunks=total_chunks,
                    uploaded_chunks=total_chunks,
                    current_chunk=total_chunks - 1,
                    phase="done",
                )
            )

        return metadata

    async def download(
        self,
        file_id_or_name: str,
        output_path: str | Path | None = None,
        password: str | None = None,
        progress_callback: ProgressCallback | None = None,
        if_exists: str = "overwrite",  # "overwrite", "skip", "rename"
    ) -> Path:
        """
        Download a file from TeleVault.

        Args:
            file_id_or_name: File ID or name to download
            output_path: Output path (uses original filename in current dir if not provided)
            password: Decryption password
            progress_callback: Optional progress callback
            if_exists: Behavior when output file exists:
                      - "overwrite": Overwrite existing file (default)
                      - "skip": Skip download, return existing path
                      - "rename": Append suffix to avoid conflict

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
        if progress_callback:
            progress_callback(
                DownloadProgress(
                    file_name=file_id_or_name,
                    total_size=0,
                    downloaded_size=0,
                    total_chunks=0,
                    downloaded_chunks=0,
                    current_chunk=0,
                    phase="metadata",
                )
            )

        metadata = await self.telegram.get_metadata(metadata_msg_id)

        # Determine output path
        output_path = Path(output_path) if output_path else Path.cwd() / metadata.name

        # Handle existing files
        if output_path.exists():
            if if_exists == "skip":
                return output_path
            elif if_exists == "rename":
                base = output_path.stem
                ext = output_path.suffix
                suffix = 1
                while output_path.exists():
                    output_path = output_path.parent / f"{base}_{suffix}{ext}"
                    suffix += 1

        # Create parent directories if needed
        output_path.parent.mkdir(parents=True, exist_ok=True)

        # Create chunk writer
        writer = ChunkWriter(output_path, metadata.size, self.config.chunk_size)

        downloaded_size = 0
        total_chunks = len(metadata.chunks)

        if progress_callback:
            progress_callback(
                DownloadProgress(
                    file_name=metadata.name,
                    total_size=metadata.size,
                    downloaded_size=0,
                    total_chunks=total_chunks,
                    downloaded_chunks=0,
                    current_chunk=0,
                    phase="downloading",
                )
            )

        # Parallel download with configurable concurrency
        semaphore = asyncio.Semaphore(self.config.parallel_downloads)
        chunk_data: dict[int, bytes] = {}
        download_lock = asyncio.Lock()

        async def download_single_chunk(chunk_info):
            nonlocal downloaded_size

            async with semaphore:
                data = await self.telegram.download_chunk(chunk_info.message_id)

                # Verify post-processing hash (encrypted/compressed) using async hashing
                if await hash_data_async(data) != chunk_info.hash:
                    raise ValueError(
                        f"Chunk {chunk_info.index} hash mismatch - data may be corrupted in transit"
                    )

                # Decrypt if needed
                if metadata.encrypted:
                    if not password:
                        raise ValueError("File is encrypted but no password provided")
                    data = decrypt_chunk(data, password)

                # Decompress if needed
                if metadata.compressed:
                    data = decompress_data(data)

                # Verify original hash if available using async hashing
                if chunk_info.original_hash:
                    computed = await hash_data_async(data)
                    if computed != chunk_info.original_hash:
                        logger.warning(
                            f"Chunk {chunk_info.index} original hash mismatch - "
                            f"decryption may have produced incorrect data"
                        )

                from .chunker import Chunk

                async with download_lock:
                    writer.write_chunk(
                        Chunk(
                            index=chunk_info.index,
                            data=data,
                            hash="",
                            size=len(data),
                        )
                    )
                    downloaded_size += len(data)
                    chunk_data[chunk_info.index] = data

                    # Progress callback
                    if progress_callback:
                        progress_callback(
                            DownloadProgress(
                                file_name=metadata.name,
                                total_size=metadata.size,
                                downloaded_size=downloaded_size,
                                total_chunks=total_chunks,
                                downloaded_chunks=len(chunk_data),
                                current_chunk=chunk_info.index,
                                phase="downloading",
                            )
                        )

        # Download all chunks in parallel
        # In low-resource mode, process chunks sequentially to reduce memory pressure
        sorted_chunks = sorted(metadata.chunks, key=lambda c: c.index)
        
        if self.config.low_resource_mode and len(sorted_chunks) > 5:
            logger.info(
                f"Low-resource mode: Processing {len(sorted_chunks)} chunks sequentially "
                "to reduce memory pressure"
            )
            # Process chunks one at a time instead of all at once
            results = []
            for chunk in sorted_chunks:
                try:
                    result = await download_single_chunk(chunk)
                    results.append(result)
                except Exception as e:
                    results.append(e)
        else:
            results = await asyncio.gather(
                *[download_single_chunk(c) for c in sorted_chunks],
                return_exceptions=True,
            )

        # Check for download errors
        for r in results:
            if isinstance(r, Exception):
                writer.close()
                raise r

        # Verify final hash
        if progress_callback:
            progress_callback(
                DownloadProgress(
                    file_name=metadata.name,
                    total_size=metadata.size,
                    downloaded_size=metadata.size,
                    total_chunks=total_chunks,
                    downloaded_chunks=total_chunks,
                    current_chunk=total_chunks - 1,
                    phase="verifying",
                )
            )

        try:
            # Use async hashing for final verification
            if await hash_file_async(output_path) != metadata.hash:
                raise ValueError(
                    "Downloaded file hash mismatch - downloaded data is corrupted; "
                    "try re-downloading or checking your network/Telegram storage."
                )
        except ValueError:
            writer.close()
            if output_path.exists():
                output_path.unlink()
            raise

        writer.close()

        if progress_callback:
            progress_callback(
                DownloadProgress(
                    file_name=metadata.name,
                    total_size=metadata.size,
                    downloaded_size=metadata.size,
                    total_chunks=total_chunks,
                    downloaded_chunks=total_chunks,
                    current_chunk=total_chunks - 1,
                    phase="done",
                )
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

        # Search by name (case-insensitive)
        files = await self.telegram.list_files()
        matches = [f for f in files if f.name.lower() == file_id_or_name.lower()]

        if not matches:
            return False
        if len(matches) > 1:
            # Return exact match if exists, otherwise error
            exact = [f for f in matches if f.name == file_id_or_name]
            if exact:
                return await self.telegram.delete_file(exact[0].id)
            raise ValueError(
                f"Multiple files match '{file_id_or_name}' (case-insensitive): "
                f"{[f.name for f in matches]}"
            )

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

    async def upload_resume(
        self,
        file_path: str | Path,
        password: str | None = None,
        progress_callback: ProgressCallback | None = None,
        preserve_path: bool = False,
    ) -> FileMetadata:
        """
        Upload a file with ability to resume if interrupted.

        This method saves progress after each chunk, allowing resumption.
        If an incomplete upload exists for the same file, it will resume.
        """
        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        file_path = Path(file_path)
        if not file_path.exists():
            raise FileNotFoundError(f"File not found: {file_path}")

        password = password or self.password
        file_name = file_path.name
        if preserve_path:
            file_name = str(file_path).replace("/", "_")

        file_size = file_path.stat().st_size
        file_hash = hash_file(file_path)
        file_id = generate_file_id(file_name, file_size)

        chunk_size = self.config.chunk_size
        total_chunks = (file_size + chunk_size - 1) // chunk_size
        if total_chunks == 0:
            total_chunks = 1

        existing_metadata = None
        existing_msg_id = None
        completed_chunks: set[int] = set()

        # Match by hash first (most reliable), then by name+size (fallback)
        for metadata in await self.telegram.list_files():
            if metadata.name == file_name and metadata.message_id:
                # Exact match by hash (same file, possibly interrupted upload)
                if metadata.hash == file_hash:
                    existing_metadata = metadata
                    existing_msg_id = metadata.message_id
                    completed_chunks = {c.index for c in metadata.chunks}
                    break
                # Fallback: same name and size (might be different content)
                elif metadata.size == file_size and not existing_metadata:
                    logger.warning(
                        f"Found incomplete upload '{file_name}' with same size but different hash. "
                        f"Resuming may produce incorrect results if file content changed."
                    )
                    existing_metadata = metadata
                    existing_msg_id = metadata.message_id
                    completed_chunks = {c.index for c in metadata.chunks}

        if existing_metadata and existing_metadata.is_complete():
            return existing_metadata

        metadata = existing_metadata or FileMetadata(
            id=file_id,
            name=file_name,
            size=file_size,
            hash=file_hash,
            encrypted=self.config.encryption and password is not None,
            compressed=self.config.compression and should_compress(file_name),
        )

        if not existing_msg_id:
            existing_msg_id = await self.telegram.upload_metadata(metadata)
            metadata.message_id = existing_msg_id
        else:
            metadata.message_id = existing_msg_id

        chunk_results: dict[int, ChunkInfo] = {c.index: c for c in metadata.chunks}
        uploaded_count = len(completed_chunks)
        lock = asyncio.Lock()

        async def upload_single_chunk(chunk):
            nonlocal uploaded_count

            if chunk.index in completed_chunks:
                return

            data = chunk.data

            if metadata.compressed:
                data = compress_data(data)

            if metadata.encrypted and password:
                data = encrypt_chunk(data, password)

            chunk_msg_id = await self.telegram.upload_chunk(
                data=data,
                filename=f"{file_id}_{chunk.index:04d}.chunk",
                reply_to=existing_msg_id,
            )

            chunk_info = ChunkInfo(
                index=chunk.index,
                message_id=chunk_msg_id,
                size=len(data),
                hash=hash_data(data),
            )

            async with lock:
                chunk_results[chunk.index] = chunk_info
                completed_chunks.add(chunk.index)
                uploaded_count += 1

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

        semaphore = asyncio.Semaphore(self.config.parallel_uploads)

        async def upload_with_limit(chunk):
            async with semaphore:
                await upload_single_chunk(chunk)

        chunks = list(iter_chunks(file_path, chunk_size))

        if chunks:
            pending = [c for c in chunks if c.index not in completed_chunks]
            if pending:
                await asyncio.gather(*[upload_with_limit(c) for c in pending])

        metadata.chunks = [chunk_results[i] for i in sorted(chunk_results.keys())]
        await self.telegram.update_metadata(existing_msg_id, metadata)

        async with self._index_lock:
            max_index_retries = 3
            for attempt in range(max_index_retries):
                try:
                    index = await self.telegram.get_index()
                    if file_id not in index.files:
                        index.add_file(file_id, existing_msg_id)
                    await self.telegram.save_index(index)
                    break
                except Exception as e:
                    if attempt >= max_index_retries - 1:
                        raise
                    logger.warning(f"Index save retry {attempt + 1}: {e}")
                    await asyncio.sleep(0.5 * (attempt + 1))

        return metadata

    async def download_resume(
        self,
        file_id_or_name: str,
        output_path: str | Path | None = None,
        password: str | None = None,
        progress_callback: ProgressCallback | None = None,
    ) -> Path:
        """
        Download a file with ability to resume if interrupted.

        This method tracks which chunks have been downloaded, allowing resumption.
        """
        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        password = password or self.password

        index = await self.telegram.get_index()

        if file_id_or_name in index.files:
            metadata_msg_id = index.files[file_id_or_name]
        else:
            files = await self.telegram.list_files()
            matches = [f for f in files if f.name == file_id_or_name or file_id_or_name in f.name]

            if not matches:
                raise FileNotFoundError(f"File not found: {file_id_or_name}")
            if len(matches) > 1:
                raise ValueError(
                    f"Multiple files match '{file_id_or_name}': {[f.name for f in matches]}"
                )

            metadata_msg_id = matches[0].message_id

        metadata = await self.telegram.get_metadata(metadata_msg_id)
        output_path = Path(output_path) if output_path else Path.cwd() / metadata.name
        output_path.parent.mkdir(parents=True, exist_ok=True)

        temp_path = output_path.with_suffix(output_path.suffix + ".partial")
        progress_file = output_path.with_suffix(output_path.suffix + ".progress")

        completed_chunks: set[int] = set()
        if temp_path.exists() and progress_file.exists():
            saved_progress = load_progress_with_crc(progress_file)
            if saved_progress is not None:
                completed_chunks = set(saved_progress.completed_chunks)
                logger.info(
                    f"Resuming download: {len(completed_chunks)}/{len(metadata.chunks)} "
                    f"chunks already completed"
                )
            else:
                logger.warning("Corrupted progress file, starting download from scratch")
                completed_chunks = set()

        writer = ChunkWriter(temp_path, metadata.size, self.config.chunk_size)

        for chunk_info in sorted(metadata.chunks, key=lambda c: c.index):
            if chunk_info.index in completed_chunks:
                continue

            data = await self.telegram.download_chunk(chunk_info.message_id)

            if hash_data(data) != chunk_info.hash:
                logger.warning(
                    f"Chunk {chunk_info.index} hash mismatch for {metadata.name} - "
                    f"retry download to resume from this chunk"
                )
                raise ValueError(
                    f"Chunk {chunk_info.index} hash mismatch - data corrupted. "
                    f"Progress saved; retry to resume."
                )

            if metadata.encrypted:
                if not password:
                    raise ValueError("File is encrypted but no password provided")
                data = decrypt_chunk(data, password)

            if metadata.compressed:
                data = decompress_data(data)

            from .chunker import Chunk

            writer.write_chunk(
                Chunk(
                    index=chunk_info.index,
                    data=data,
                    hash="",
                    size=len(data),
                )
            )

            completed_chunks.add(chunk_info.index)

            progress = TransferProgress(
                operation="download",
                file_id=metadata.id,
                file_name=metadata.name,
                total_chunks=len(metadata.chunks),
                completed_chunks=list(completed_chunks),
            )
            save_progress_with_crc(progress, progress_file)

            if progress_callback:
                progress_callback(
                    DownloadProgress(
                        file_name=metadata.name,
                        total_size=metadata.size,
                        downloaded_size=sum(
                            c.size for c in metadata.chunks if c.index in completed_chunks
                        ),
                        total_chunks=len(metadata.chunks),
                        downloaded_chunks=len(completed_chunks),
                        current_chunk=chunk_info.index,
                    )
                )

        try:
            if hash_file(temp_path) != metadata.hash:
                raise ValueError(
                    "Downloaded file hash mismatch - downloaded data is corrupted. "
                    "Partial progress saved; retry to resume."
                )
        except ValueError:
            raise

        temp_path.rename(output_path)
        progress_file.unlink(missing_ok=True)

        return output_path

    async def stream(
        self,
        file_id_or_name: str,
        output=None,
        password: str | None = None,
        progress_callback: ProgressCallback | None = None,
    ):
        """
        Stream a file's content to an output stream (supports stdout for piping).

        Args:
            file_id_or_name: File ID or name to stream
            output: Output stream (defaults to sys.stdout.buffer)
            password: Decryption password
            progress_callback: Optional progress callback

        Returns:
            Total bytes written
        """
        import sys

        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        password = password or self.password
        if output is None:
            output = sys.stdout.buffer

        index = await self.telegram.get_index()

        if file_id_or_name in index.files:
            metadata_msg_id = index.files[file_id_or_name]
        else:
            files = await self.telegram.list_files()
            matches = [f for f in files if f.name == file_id_or_name or file_id_or_name in f.name]

            if not matches:
                raise FileNotFoundError(f"File not found: {file_id_or_name}")
            if len(matches) > 1:
                raise ValueError(
                    f"Multiple files match '{file_id_or_name}': {[f.name for f in matches]}"
                )

            metadata_msg_id = matches[0].message_id

        metadata = await self.telegram.get_metadata(metadata_msg_id)

        total_size = 0

        for chunk_info in sorted(metadata.chunks, key=lambda c: c.index):
            data = await self.telegram.download_chunk(chunk_info.message_id)

            if hash_data(data) != chunk_info.hash:
                raise ValueError(f"Chunk {chunk_info.index} hash mismatch")

            if metadata.encrypted:
                if not password:
                    raise ValueError("File is encrypted but no password provided")
                data = decrypt_chunk(data, password)

            if metadata.compressed:
                data = decompress_data(data)

            if chunk_info.original_hash and hash_data(data) != chunk_info.original_hash:
                logger.warning(f"Chunk {chunk_info.index} original hash mismatch")

            output.write(data)
            output.flush()
            total_size += len(data)

            if progress_callback:
                progress_callback(
                    DownloadProgress(
                        file_name=metadata.name,
                        total_size=metadata.size,
                        downloaded_size=total_size,
                        total_chunks=len(metadata.chunks),
                        downloaded_chunks=chunk_info.index + 1,
                        current_chunk=chunk_info.index,
                    )
                )

        return total_size

    async def upload_stream(
        self,
        data: bytes,
        filename: str,
        password: str | None = None,
        progress_callback: ProgressCallback | None = None,
    ) -> FileMetadata:
        """
        Upload data from a byte stream (e.g., stdin pipe).

        Note: For large data, prefer writing to temp file first to avoid memory issues.

        Args:
            data: Raw bytes to upload
            filename: Name for the uploaded file
            password: Encryption password
            progress_callback: Optional progress callback

        Returns:
            FileMetadata of uploaded file
        """
        if not self._connected:
            raise RuntimeError("Not connected. Call connect() first.")

        password = password or self.password

        import tempfile

        with tempfile.NamedTemporaryFile(
            dir=tempfile.gettempdir(),
            prefix=f"televault_{filename}_",
            suffix=".tmp",
            delete=False,
        ) as tmp:
            tmp.write(data)
            tmp_path = Path(tmp.name)

        try:
            metadata = await self.upload(
                tmp_path,
                password=password,
                progress_callback=progress_callback,
                name=filename,
            )
            return metadata
        finally:
            tmp_path.unlink(missing_ok=True)
