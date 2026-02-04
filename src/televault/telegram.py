"""Telegram MTProto client wrapper for TeleVault."""

import asyncio
import json
from pathlib import Path
from typing import Optional, AsyncIterator
from dataclasses import dataclass
import io

from telethon import TelegramClient
from telethon.sessions import StringSession
from telethon.tl.types import (
    Channel,
    Message,
    DocumentAttributeFilename,
    InputPeerChannel,
)
from telethon.tl.functions.messages import GetPinnedDialogsRequest
from telethon.errors import FloodWaitError

from .models import FileMetadata, VaultIndex, ChunkInfo, TransferProgress
from .config import Config, get_config_dir


# TeleVault Telegram app credentials
API_ID = 22399403
API_HASH = "9bf0e01ba1d63bc048172b8eb53d957b"


@dataclass
class TelegramConfig:
    """Telegram connection configuration."""
    
    api_id: int
    api_hash: str
    session_string: Optional[str] = None
    
    @classmethod
    def from_env(cls) -> "TelegramConfig":
        """Load from environment or config file."""
        import os
        
        config_path = get_config_dir() / "telegram.json"
        
        if config_path.exists():
            with open(config_path) as f:
                data = json.load(f)
                return cls(**data)
        
        return cls(
            api_id=int(os.environ.get("TELEGRAM_API_ID", API_ID)),
            api_hash=os.environ.get("TELEGRAM_API_HASH", API_HASH),
            session_string=os.environ.get("TELEGRAM_SESSION"),
        )
    
    def save(self) -> None:
        """Save config to file."""
        config_path = get_config_dir() / "telegram.json"
        config_path.parent.mkdir(parents=True, exist_ok=True)
        
        with open(config_path, "w") as f:
            json.dump({
                "api_id": self.api_id,
                "api_hash": self.api_hash,
                "session_string": self.session_string,
            }, f, indent=2)


class TelegramVault:
    """
    Telegram MTProto client for TeleVault operations.
    
    Handles:
    - Authentication
    - Channel management
    - File upload/download
    - Index management
    """
    
    def __init__(self, config: Optional[TelegramConfig] = None):
        self.config = config or TelegramConfig.from_env()
        self._client: Optional[TelegramClient] = None
        self._channel: Optional[Channel] = None
        self._channel_id: Optional[int] = None
    
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
    
    async def login(self, phone: Optional[str] = None) -> str:
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
            except Exception:
                # 2FA required
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
    
    async def create_channel(self, name: str = "TeleVault Storage") -> int:
        """Create a new private channel for storage."""
        from telethon.tl.functions.channels import CreateChannelRequest
        
        result = await self._client(CreateChannelRequest(
            title=name,
            about="TeleVault encrypted storage",
            megagroup=False,  # Regular channel, not supergroup
        ))
        
        channel = result.chats[0]
        self._channel = channel
        self._channel_id = channel.id
        
        # Return full channel ID format (negative with -100 prefix)
        # Telegram channels need -100 prefix for MTProto
        full_channel_id = int(f"-100{channel.id}")
        return full_channel_id
    
    # === Index Operations ===
    
    async def get_index(self) -> VaultIndex:
        """Get the vault index from pinned message."""
        if not self._channel_id:
            raise ValueError("No channel set")
        
        # Get pinned messages
        async for msg in self._client.iter_messages(
            self._channel_id,
            filter=None,
            limit=10,
        ):
            if msg.pinned and msg.text:
                try:
                    data = json.loads(msg.text)
                    # Check if it looks like our index (has 'files' key)
                    if "files" in data:
                        return VaultIndex.from_json(msg.text)
                except json.JSONDecodeError:
                    continue
        
        # No valid index found, create empty one
        return VaultIndex()
    
    async def save_index(self, index: VaultIndex) -> int:
        """Save the vault index as pinned message."""
        if not self._channel_id:
            raise ValueError("No channel set")
        
        # Find existing pinned index message
        existing_msg_id = None
        async for msg in self._client.iter_messages(
            self._channel_id,
            filter=None,
            limit=10,
        ):
            if msg.pinned and msg.text:
                try:
                    VaultIndex.from_json(msg.text)
                    existing_msg_id = msg.id
                    break
                except json.JSONDecodeError:
                    continue
        
        if existing_msg_id:
            # Edit existing
            await self._client.edit_message(
                self._channel_id,
                existing_msg_id,
                index.to_json(),
            )
            return existing_msg_id
        else:
            # Create new and pin
            msg = await self._client.send_message(
                self._channel_id,
                index.to_json(),
            )
            await self._client.pin_message(self._channel_id, msg.id)
            return msg.id
    
    # === File Operations ===
    
    async def upload_metadata(self, metadata: FileMetadata) -> int:
        """Upload file metadata as a text message."""
        if not self._channel_id:
            raise ValueError("No channel set")
        
        msg = await self._client.send_message(
            self._channel_id,
            metadata.to_json(),
        )
        return msg.id
    
    async def get_metadata(self, message_id: int) -> FileMetadata:
        """Get file metadata from message."""
        if not self._channel_id:
            raise ValueError("No channel set")
        
        msg = await self._client.get_messages(self._channel_id, ids=message_id)
        if not msg or not msg.text:
            raise ValueError(f"Metadata message {message_id} not found")
        
        return FileMetadata.from_json(msg.text)
    
    async def update_metadata(self, message_id: int, metadata: FileMetadata) -> None:
        """Update file metadata message."""
        if not self._channel_id:
            raise ValueError("No channel set")
        
        await self._client.edit_message(
            self._channel_id,
            message_id,
            metadata.to_json(),
        )
    
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
        
        # Create file-like object
        file = io.BytesIO(data)
        file.name = filename
        
        try:
            msg = await self._client.send_file(
                self._channel_id,
                file,
                reply_to=reply_to,
                progress_callback=progress_callback,
                attributes=[DocumentAttributeFilename(filename)],
            )
            return msg.id
        except FloodWaitError as e:
            # Rate limited, wait and retry
            await asyncio.sleep(e.seconds + 1)
            return await self.upload_chunk(data, filename, reply_to, progress_callback)
    
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
        
        return await self._client.download_media(msg, file=bytes, progress_callback=progress_callback)
    
    async def iter_file_chunks(self, metadata_msg_id: int) -> AsyncIterator[Message]:
        """Iterate over chunk messages that reply to a metadata message."""
        if not self._channel_id:
            raise ValueError("No channel set")
        
        async for msg in self._client.iter_messages(
            self._channel_id,
            reply_to=metadata_msg_id,
        ):
            if msg.file:
                yield msg
    
    async def delete_file(self, file_id: str) -> bool:
        """Delete a file and all its chunks."""
        if not self._channel_id:
            raise ValueError("No channel set")
        
        # Get index
        index = await self.get_index()
        
        if file_id not in index.files:
            return False
        
        metadata_msg_id = index.files[file_id]
        
        # Collect all message IDs to delete
        msg_ids = [metadata_msg_id]
        
        async for chunk_msg in self.iter_file_chunks(metadata_msg_id):
            msg_ids.append(chunk_msg.id)
        
        # Delete messages
        await self._client.delete_messages(self._channel_id, msg_ids)
        
        # Update index
        index.remove_file(file_id)
        await self.save_index(index)
        
        return True
    
    # === Listing ===
    
    async def list_files(self) -> list[FileMetadata]:
        """List all files in the vault."""
        index = await self.get_index()
        files = []
        
        for file_id, msg_id in index.files.items():
            try:
                metadata = await self.get_metadata(msg_id)
                metadata.message_id = msg_id
                files.append(metadata)
            except Exception:
                # Skip corrupted entries
                continue
        
        return files
    
    async def search_files(self, query: str) -> list[FileMetadata]:
        """Search files by name."""
        files = await self.list_files()
        query_lower = query.lower()
        
        return [f for f in files if query_lower in f.name.lower()]
