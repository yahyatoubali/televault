"""Core TeleVault operations - upload, download, list."""

import asyncio
import contextlib
import hashlib
import logging
import os
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from .chunker import ChunkWriter, hash_data, hash_file, iter_chunks
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
        self._index_lock = asyncio.Lock()

    async def is_authenticated(self) -> bool:
        """Check if user is authenticated with Telegram."""
        return await self.telegram._client.is_user_authorized()

    async def get_account_info(self) -> dict:
        """Get current account info."""
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
        return await self.telegram.test_channel(channel_id)

    async def list_channels(self) -> list[dict]:
        """List all channels the user is a member of."""
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
        uploaded_msg_ids: list[int] = []  # Track for cleanup on failure

        async def upload_single_chunk(chunk):
            nonlocal uploaded_count

            data = chunk.data
            original_hash = hash_data(data)

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
                original_hash=original_hash,
            )

            async with lock:
                chunk_results[chunk.index] = chunk_info
                uploaded_msg_ids.append(chunk_msg_id)
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
            try:
                await asyncio.gather(*[upload_with_limit(c) for c in chunks])
            except Exception:
                # Cleanup on failure: delete uploaded chunks and metadata
                logger.error(f"Upload failed for {file_name}, cleaning up...")
                all_ids = uploaded_msg_ids + [metadata_msg_id]
                with contextlib.suppress(Exception):
                    await self.telegram._client.delete_messages(self.telegram._channel_id, all_ids)
                raise

        # Sort chunks by index
        metadata.chunks = [chunk_results[i] for i in sorted(chunk_results.keys())]

        # Update metadata with chunk info
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

        # Create parent directories if needed
        output_path.parent.mkdir(parents=True, exist_ok=True)

        # Create chunk writer
        writer = ChunkWriter(output_path, metadata.size, self.config.chunk_size)

        downloaded_size = 0
        total_chunks = len(metadata.chunks)

        # Parallel download with configurable concurrency
        semaphore = asyncio.Semaphore(self.config.parallel_downloads)
        chunk_data: dict[int, bytes] = {}
        download_lock = asyncio.Lock()

        async def download_single_chunk(chunk_info):
            nonlocal downloaded_size

            async with semaphore:
                data = await self.telegram.download_chunk(chunk_info.message_id)

                # Verify post-processing hash (encrypted/compressed)
                if hash_data(data) != chunk_info.hash:
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

                # Verify original hash if available
                if chunk_info.original_hash and hash_data(data) != chunk_info.original_hash:
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
                            )
                        )

        # Download all chunks in parallel
        sorted_chunks = sorted(metadata.chunks, key=lambda c: c.index)
        results = await asyncio.gather(
            *[download_single_chunk(c) for c in sorted_chunks],
            return_exceptions=True,
        )

        # Check for download errors
        for r in results:
            if isinstance(r, Exception):
                raise r

        # Verify final hash
        try:
            if hash_file(output_path) != metadata.hash:
                raise ValueError(
                    "Downloaded file hash mismatch - downloaded data is corrupted; "
                    "try re-downloading or checking your network/Telegram storage."
                )
        except ValueError:
            if output_path.exists():
                output_path.unlink()
            raise

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

        for metadata in await self.telegram.list_files():
            if metadata.name == file_name and metadata.size == file_size and metadata.message_id:
                existing_metadata = metadata
                existing_msg_id = metadata.message_id
                completed_chunks = {c.index for c in metadata.chunks}
                break

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
                temp_path.unlink(missing_ok=True)
                progress_file.unlink(missing_ok=True)
                raise ValueError(f"Chunk {chunk_info.index} hash mismatch - data corrupted")

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
                temp_path.unlink(missing_ok=True)
                progress_file.unlink(missing_ok=True)
                raise ValueError("Downloaded file hash mismatch - downloaded data is corrupted")
        except ValueError:
            temp_path.unlink(missing_ok=True)
            progress_file.unlink(missing_ok=True)
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
            )
            metadata.name = filename

            await self.telegram.update_metadata(metadata.message_id, metadata)

            async with self._index_lock:
                max_index_retries = 3
                for attempt in range(max_index_retries):
                    try:
                        index = await self.telegram.get_index()
                        if metadata.id not in index.files:
                            index.add_file(metadata.id, metadata.message_id)
                        await self.telegram.save_index(index)
                        break
                    except Exception as e:
                        if attempt >= max_index_retries - 1:
                            raise
                        logger.warning(f"Index save retry {attempt + 1}: {e}")
                        await asyncio.sleep(0.5 * (attempt + 1))

            return metadata
        finally:
            tmp_path.unlink(missing_ok=True)
