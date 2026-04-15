"""Telegram MTProto client wrapper for TeleVault."""

import asyncio
import base64
import contextlib
import io
import json
import logging
import zlib
from collections.abc import AsyncIterator
from dataclasses import dataclass
from datetime import datetime

from telethon import TelegramClient
from telethon.errors import SessionPasswordNeededError
from telethon.sessions import StringSession
from telethon.tl.types import (
    Channel,
    DocumentAttributeFilename,
    Message,
)

from .config import Config, get_config_dir
from .models import FileMetadata, VaultIndex
from .retry import with_retry

logger = logging.getLogger("televault.telegram")

TELEGRAM_MSG_LIMIT = 4096
TV_PREFIX = "__TV1__"


def _compress_message(text: str) -> str:
    """Compress text to fit Telegram's message size limit."""
    if len(text) <= TELEGRAM_MSG_LIMIT:
        return text
    compressed = zlib.compress(text.encode("utf-8"), 9)
    encoded = base64.b64encode(compressed).decode("ascii")
    return f"{TV_PREFIX}{encoded}"


def _decompress_message(text: str) -> str:
    """Decompress text that was compressed by _compress_message."""
    if not text.startswith(TV_PREFIX):
        return text
    encoded = text[len(TV_PREFIX) :]
    compressed = base64.b64decode(encoded)
    return zlib.decompress(compressed).decode("utf-8")


# TeleVault Telegram app credentials
# Users must provide their own from https://my.telegram.org
# Set via environment variables:
#   export TELEGRAM_API_ID=your_api_id
#   export TELEGRAM_API_HASH=your_api_hash


@dataclass
class TelegramConfig:
    """Telegram connection configuration."""

    api_id: int
    api_hash: str
    session_string: str | None = None

    @classmethod
    def from_env(cls) -> "TelegramConfig":
        """Load from environment or config file."""
        import os

        config_path = get_config_dir() / "telegram.json"

        if config_path.exists():
            with open(config_path) as f:
                data = json.load(f)
                return cls(**data)

        # Load from environment variables (required)
        api_id = os.environ.get("TELEGRAM_API_ID")
        api_hash = os.environ.get("TELEGRAM_API_HASH")

        if not api_id or not api_hash:
            raise ValueError(
                "Telegram API credentials not found.\n"
                "Please set environment variables:\n"
                "  export TELEGRAM_API_ID=your_api_id\n"
                "  export TELEGRAM_API_HASH=your_api_hash\n\n"
                "Get your credentials at: https://my.telegram.org"
            )

        return cls(
            api_id=int(api_id),
            api_hash=api_hash,
            session_string=os.environ.get("TELEGRAM_SESSION"),
        )

    def save(self) -> None:
        """Save config to file."""
        config_path = get_config_dir() / "telegram.json"
        config_path.parent.mkdir(parents=True, exist_ok=True)

        with open(config_path, "w") as f:
            json.dump(
                {
                    "api_id": self.api_id,
                    "api_hash": self.api_hash,
                    "session_string": self.session_string,
                },
                f,
                indent=2,
            )


class TelegramVault:
    """
    Telegram MTProto client for TeleVault operations.

    Handles:
    - Authentication
    - Channel management
    - File upload/download
    - Index management
    """

    def __init__(self, config: TelegramConfig | None = None):
        self.config = config or TelegramConfig.from_env()
        self._client: TelegramClient | None = None
        self._channel: Channel | None = None
        self._channel_id: int | None = None
        self._index_msg_id: int | None = None

    async def connect(self) -> None:
        """Connect to Telegram."""
        if self.config.session_string:
            session = StringSession(self.config.session_string)
        else:
            session = StringSession()

        self._client = TelegramClient(
            session,
            self.config.api_id,
            self.config.api_hash,
        )

        await self._client.connect()

    async def disconnect(self) -> None:
        """Disconnect from Telegram."""
        if self._client:
            await self._client.disconnect()

    async def login(self, phone: str | None = None) -> str:
        """
        Interactive login flow.

        Returns session string for future use.
        """
        if not self._client:
            await self.connect()

        if not await self._client.is_user_authorized():
            if phone is None:
                phone = input("Enter phone number: ")

            await self._client.send_code_request(phone)
            code = input("Enter the code you received: ")

            try:
                await self._client.sign_in(phone, code)
            except SessionPasswordNeededError:
                password = input("Enter 2FA password: ")
                await self._client.sign_in(password=password)

        # Save session
        session_string = self._client.session.save()
        self.config.session_string = session_string
        self.config.save()

        return session_string

    async def set_channel(self, channel_id: int) -> None:
        """Set the storage channel."""
        self._channel_id = channel_id
        self._channel = await self._client.get_entity(channel_id)

    async def test_channel(self, channel_id: int) -> dict:
        """Test if a channel is accessible and writable.

        Returns dict with: accessible, writable, title, channel_id
        """
        from telethon.tl.functions.channels import GetFullChannelRequest

        result = {
            "accessible": False,
            "writable": False,
            "title": None,
            "channel_id": channel_id,
            "username": None,
        }

        try:
            entity = await self._client.get_entity(channel_id)
            result["accessible"] = True
            result["title"] = getattr(entity, "title", None)
            result["username"] = getattr(entity, "username", None)

            try:
                full = await self._client(GetFullChannelRequest(entity))
                result["member_count"] = getattr(full.full_chat, "participants_count", None)
            except Exception:
                pass

            try:
                msg = await self._client.send_message(entity, "__televault_test__")
                await self._client.delete_messages(entity, msg)
                result["writable"] = True
            except Exception:
                pass

        except Exception:
            pass

        return result

    async def list_channels(self) -> list[dict]:
        """List all channels the user is a member of (admin or owner)."""
        channels = []
        async for dialog in self._client.iter_dialogs():
            if dialog.is_channel and not dialog.is_group:
                entity = dialog.entity
                is_admin = False
                if hasattr(entity, "admin_rights") and entity.admin_rights:
                    is_admin = True
                if hasattr(entity, "creator") and entity.creator:
                    is_admin = True
                channels.append(
                    {
                        "id": int(f"-100{entity.id}"),
                        "title": dialog.title or dialog.name,
                        "username": getattr(entity, "username", None),
                        "is_admin": is_admin,
                    }
                )
        return channels

    async def create_channel(self, name: str = "TeleVault Storage") -> int:
        """Create a new private channel for storage."""
        from telethon.tl.functions.channels import CreateChannelRequest

        result = await self._client(
            CreateChannelRequest(
                title=name,
                about="TeleVault encrypted storage",
                megagroup=False,  # Regular channel, not supergroup
            )
        )

        channel = result.chats[0]
        self._channel = channel
        self._channel_id = channel.id

        # Return full channel ID format (negative with -100 prefix)
        # Telegram channels need -100 prefix for MTProto
        full_channel_id = int(f"-100{channel.id}")
        return full_channel_id

    # === Index Operations ===

    async def get_index(self) -> VaultIndex:
        """Get the vault index from pinned message.

        Uses cached index_msg_id for O(1) lookup.
        Falls back to scanning pinned messages if not cached.
        """
        if not self._channel_id:
            raise ValueError("No channel set")

        if self._index_msg_id:
            try:
                msg = await self._client.get_messages(self._channel_id, ids=self._index_msg_id)
                if msg and msg.text:
                    text = _decompress_message(msg.text)
                    data = json.loads(text)
                    if "files" in data:
                        return VaultIndex.from_json(text)
            except Exception:
                pass

        config = Config.load()
        if config.index_msg_id and config.index_msg_id != self._index_msg_id:
            try:
                msg = await self._client.get_messages(self._channel_id, ids=config.index_msg_id)
                if msg and msg.text:
                    text = _decompress_message(msg.text)
                    data = json.loads(text)
                    if "files" in data:
                        self._index_msg_id = msg.id
                        return VaultIndex.from_json(text)
            except Exception:
                pass

        async for msg in self._client.iter_messages(
            self._channel_id,
            filter=None,
            limit=None,
        ):
            if msg.pinned and msg.text:
                try:
                    text = _decompress_message(msg.text)
                    data = json.loads(text)
                    if "files" in data:
                        self._index_msg_id = msg.id
                        self._save_index_msg_id(msg.id)
                        return VaultIndex.from_json(text)
                except json.JSONDecodeError:
                    continue

        return VaultIndex()

    async def save_index(self, index: VaultIndex) -> int:
        """Save the vault index, using cached message ID for fast lookup."""
        if not self._channel_id:
            raise ValueError("No channel set")

        index.updated_at = datetime.now().timestamp()

        if not self._index_msg_id:
            config = Config.load()
            self._index_msg_id = config.index_msg_id

        if self._index_msg_id:
            existing_version = 0
            try:
                msg = await self._client.get_messages(self._channel_id, ids=self._index_msg_id)
                if msg and msg.text:
                    try:
                        text = _decompress_message(msg.text)
                        data = json.loads(text)
                        if "files" in data:
                            existing_version = VaultIndex.from_json(text).version
                    except json.JSONDecodeError:
                        pass
            except Exception:
                msg = None

            index.version = existing_version + 1 if existing_version else (index.version or 1)
            index_text = _compress_message(index.to_json())

            for attempt in range(3):
                try:
                    msg_check = await self._client.get_messages(
                        self._channel_id, ids=self._index_msg_id
                    )
                    if msg_check and msg_check.text:
                        await self._client.edit_message(
                            self._channel_id,
                            self._index_msg_id,
                            index_text,
                        )
                    else:
                        new_msg = await self._client.send_message(
                            self._channel_id,
                            index_text,
                        )
                        await self._client.pin_message(self._channel_id, new_msg.id)
                        self._index_msg_id = new_msg.id

                    self._save_index_msg_id(self._index_msg_id)
                    return self._index_msg_id
                except Exception as e:
                    if attempt >= 2:
                        raise
                    logger.warning(f"save_index retry {attempt + 1}/3: {e}")
                    await asyncio.sleep(0.5 * (attempt + 1))

        index.version = 1
        index_text = _compress_message(index.to_json())
        msg = await self._client.send_message(
            self._channel_id,
            index_text,
        )
        await self._client.pin_message(self._channel_id, msg.id)
        self._index_msg_id = msg.id
        self._save_index_msg_id(msg.id)
        return msg.id

    def _save_index_msg_id(self, msg_id: int) -> None:
        """Persist index message ID to config for fast lookups."""
        try:
            config = Config.load()
            if config.index_msg_id != msg_id:
                config.index_msg_id = msg_id
                config.save()
        except Exception as e:
            logger.debug(f"Failed to save index msg_id: {e}")

    # === File Operations ===

    @with_retry(max_retries=3, base_delay=1.0)
    async def upload_metadata(self, metadata: FileMetadata) -> int:
        """Upload file metadata as a text message."""
        if not self._channel_id:
            raise ValueError("No channel set")

        metadata_text = _compress_message(metadata.to_json())
        msg = await self._client.send_message(
            self._channel_id,
            metadata_text,
        )
        logger.debug(f"Uploaded metadata for {metadata.id}")
        return msg.id

    @with_retry(max_retries=3, base_delay=1.0)
    async def get_metadata(self, message_id: int) -> FileMetadata:
        """Get file metadata from message."""
        if not self._channel_id:
            raise ValueError("No channel set")

        msg = await self._client.get_messages(self._channel_id, ids=message_id)
        if not msg or not msg.text:
            raise ValueError(f"Metadata message {message_id} not found")
        text = _decompress_message(msg.text)
        return FileMetadata.from_json(text)

    @with_retry(max_retries=3, base_delay=1.0)
    async def update_metadata(self, message_id: int, metadata: FileMetadata) -> None:
        """Update file metadata message."""
        if not self._channel_id:
            raise ValueError("No channel set")

        metadata_text = _compress_message(metadata.to_json())
        await self._client.edit_message(
            self._channel_id,
            message_id,
            metadata_text,
        )

    @with_retry(max_retries=3, base_delay=1.0)
    async def upload_chunk(
        self,
        data: bytes,
        filename: str,
        reply_to: int,
        progress_callback=None,
    ) -> int:
        """
        Upload a chunk as a file message.

        Args:
            data: Chunk data
            filename: Chunk filename (e.g., "0001.chunk")
            reply_to: Metadata message ID to reply to
            progress_callback: Optional progress callback

        Returns:
            Message ID of uploaded chunk
        """
        if not self._channel_id:
            raise ValueError("No channel set")

        file = io.BytesIO(data)
        file.name = filename

        msg = await self._client.send_file(
            self._channel_id,
            file,
            reply_to=reply_to,
            progress_callback=progress_callback,
            attributes=[DocumentAttributeFilename(filename)],
        )
        return msg.id

    @with_retry(max_retries=3, base_delay=1.0)
    async def download_chunk(
        self,
        message_id: int,
        progress_callback=None,
    ) -> bytes:
        """Download a chunk by message ID."""
        if not self._channel_id:
            raise ValueError("No channel set")

        msg = await self._client.get_messages(self._channel_id, ids=message_id)
        if not msg or not msg.file:
            raise ValueError(f"Chunk message {message_id} not found")

        data = await self._client.download_media(
            msg, file=bytes, progress_callback=progress_callback
        )
        if data is None:
            raise ValueError(f"Failed to download chunk {message_id}")
        return data

    async def iter_file_chunks(self, metadata_msg_id: int) -> AsyncIterator[Message]:
        """Iterate over chunk messages that reply to a metadata message."""
        if not self._channel_id:
            raise ValueError("No channel set")

        try:
            async for msg in self._client.iter_messages(
                self._channel_id,
                reply_to=metadata_msg_id,
            ):
                if msg.file:
                    yield msg
        except Exception as e:
            logger.warning(f"Failed to iterate chunks for metadata msg {metadata_msg_id}: {e}")
            return

    async def delete_file(self, file_id: str) -> bool:
        """Delete a file and all its chunks."""
        if not self._channel_id:
            raise ValueError("No channel set")

        # Get index and remove entry FIRST (crash safety)
        index = await self.get_index()

        if file_id not in index.files:
            return False

        metadata_msg_id = index.files[file_id]

        # Read metadata to collect chunk IDs before removing from index
        chunk_msg_ids: list[int] = []
        try:
            metadata = await self.get_metadata(metadata_msg_id)
            chunk_msg_ids = [c.message_id for c in metadata.chunks]
        except Exception:
            pass

        # Remove from index BEFORE deleting messages (crash safety)
        # If crash happens after index update but before message deletion,
        # gc will clean up the orphaned messages
        index.remove_file(file_id)
        await self.save_index(index)

        # Delete messages (ignore errors for already-deleted messages)
        msg_ids = [metadata_msg_id] + chunk_msg_ids
        with contextlib.suppress(Exception):
            await self._client.delete_messages(self._channel_id, msg_ids)

        return True

    # === Listing ===

    async def list_files(self) -> list[FileMetadata]:
        """List all files in the vault."""
        index = await self.get_index()
        files = []
        stale_ids = []

        for file_id, msg_id in index.files.items():
            try:
                metadata = await self.get_metadata(msg_id)
                metadata.message_id = msg_id
                files.append(metadata)
            except Exception as e:
                logger.debug(f"Skipping corrupted index entry {file_id} (msg {msg_id}): {e}")
                stale_ids.append(file_id)

        if stale_ids:
            logger.debug(f"Cleaning {len(stale_ids)} stale index entries: {stale_ids}")
            for fid in stale_ids:
                del index.files[fid]
            try:
                await self.save_index(index)
            except Exception:
                logger.debug("Failed to clean stale entries from index")

        return files

    async def search_files(self, query: str) -> list[FileMetadata]:
        """Search files by name."""
        files = await self.list_files()
        query_lower = query.lower()

        return [f for f in files if query_lower in f.name.lower()]
