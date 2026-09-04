"""Local mock storage and Telegram channel emulator for offline E2E workflows."""

import json
import os
from pathlib import Path
from typing import Any, Dict, List, Optional


class MockTelegramChannel:
    """Emulates Telegram channel message history, document storage, and pinned index."""

    def __init__(self, storage_dir: Path, channel_id: int = -1001234567890):
        self.storage_dir = Path(storage_dir)
        self.channel_id = channel_id
        self.messages_file = self.storage_dir / "messages.json"
        self.chunks_dir = self.storage_dir / "chunks"

        self.storage_dir.mkdir(parents=True, exist_ok=True)
        self.chunks_dir.mkdir(parents=True, exist_ok=True)

        self.messages: Dict[int, Dict[str, Any]] = {}
        self.pinned_message_id: Optional[int] = None
        self._next_msg_id = 1000
        self._next_file_id = 5000

        self._load()

    def _load(self):
        if self.messages_file.exists():
            try:
                with open(self.messages_file, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    self.messages = {int(k): v for k, v in data.get("messages", {}).items()}
                    self.pinned_message_id = data.get("pinned_id")
                    self._next_msg_id = data.get("next_msg_id", 1000)
                    self._next_file_id = data.get("next_file_id", 5000)
            except Exception:
                pass

    def _save(self):
        data = {
            "messages": self.messages,
            "pinned_id": self.pinned_message_id,
            "next_msg_id": self._next_msg_id,
            "next_file_id": self._next_file_id,
        }
        with open(self.messages_file, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

    def post_text_message(self, text: str) -> int:
        msg_id = self._next_msg_id
        self._next_msg_id += 1
        self.messages[msg_id] = {
            "id": msg_id,
            "type": "text",
            "text": text,
        }
        self._save()
        return msg_id

    def post_document(self, filename: str, content: bytes) -> Dict[str, Any]:
        msg_id = self._next_msg_id
        self._next_msg_id += 1
        file_id = self._next_file_id
        self._next_file_id += 1

        stored_path = self.chunks_dir / f"chunk_{file_id}_{filename}"
        with open(stored_path, "wb") as f:
            f.write(content)

        msg_data = {
            "id": msg_id,
            "type": "document",
            "filename": filename,
            "file_id": file_id,
            "size": len(content),
            "stored_path": str(stored_path),
        }
        self.messages[msg_id] = msg_data
        self._save()
        return msg_data

    def pin_message(self, msg_id: int):
        self.pinned_message_id = msg_id
        self._save()

    def get_pinned_message(self) -> Optional[Dict[str, Any]]:
        if self.pinned_message_id and self.pinned_message_id in self.messages:
            return self.messages[self.pinned_message_id]
        return None

    def delete_messages(self, msg_ids: List[int]) -> int:
        deleted = 0
        for mid in msg_ids:
            if mid in self.messages:
                msg = self.messages.pop(mid)
                if msg.get("type") == "document" and "stored_path" in msg:
                    p = Path(msg["stored_path"])
                    if p.exists():
                        p.unlink()
                deleted += 1
        self._save()
        return deleted

    def list_documents(self) -> List[Dict[str, Any]]:
        return [m for m in self.messages.values() if m.get("type") == "document"]
