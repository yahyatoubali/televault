"""Garbage collection for orphaned Telegram messages."""

import logging

logger = logging.getLogger("televault.gc")


async def _collect_pinned_ids(telegram) -> set[int]:
    """Collect IDs of all pinned messages (index, snapshot index, etc.)."""
    pinned_ids: set[int] = set()
    if not telegram._channel_id:
        return pinned_ids

    async for msg in telegram._client.iter_messages(telegram._channel_id, filter=None, limit=50):
        if msg.pinned:
            pinned_ids.add(msg.id)

    return pinned_ids


async def collect_garbage(telegram, dry_run: bool = False) -> dict:
    """
    Find and optionally remove orphaned messages from the Telegram channel.

    Orphaned messages are chunks or metadata messages that are not referenced
    by any file in the vault index.

    Args:
        telegram: TelegramVault instance (must be connected)
        dry_run: If True, only report orphans without deleting them

    Returns:
        Dict with 'orphaned_messages', 'orphaned_size', 'deleted_count' keys
    """
    if not telegram._channel_id:
        raise ValueError("No channel set")

    logger.info("Starting garbage collection...")

    index = await telegram.get_index()
    referenced_msg_ids: set[int] = set()

    pinned_ids = await _collect_pinned_ids(telegram)
    referenced_msg_ids.update(pinned_ids)

    for file_id, msg_id in index.files.items():
        referenced_msg_ids.add(msg_id)
        try:
            metadata = await telegram.get_metadata(msg_id)
            for chunk in metadata.chunks:
                referenced_msg_ids.add(chunk.message_id)
        except Exception as e:
            logger.warning(f"Could not read metadata for {file_id} (msg {msg_id}): {e}")

    orphaned_messages = []
    total_orphaned_size = 0

    async for msg in telegram._client.iter_messages(telegram._channel_id, limit=None):
        if msg.id not in referenced_msg_ids:
            size = msg.file.size if msg.file else len(msg.text) if msg.text else 0
            total_orphaned_size += size
            orphaned_messages.append(
                {
                    "id": msg.id,
                    "type": "file" if msg.file else "text",
                    "size": size,
                    "date": msg.date.isoformat() if msg.date else None,
                }
            )

    logger.info(f"Found {len(orphaned_messages)} orphaned messages ({total_orphaned_size} bytes)")

    deleted_count = 0
    if not dry_run and orphaned_messages:
        msg_ids = [m["id"] for m in orphaned_messages]
        for i in range(0, len(msg_ids), 100):
            batch = msg_ids[i : i + 100]
            try:
                await telegram._client.delete_messages(telegram._channel_id, batch)
                deleted_count += len(batch)
                logger.info(f"Deleted batch of {len(batch)} orphaned messages")
            except Exception as e:
                logger.error(f"Failed to delete orphan batch: {e}")

    return {
        "orphaned_messages": orphaned_messages,
        "orphaned_size": total_orphaned_size,
        "deleted_count": deleted_count,
    }


async def cleanup_partial_uploads(telegram) -> int:
    """
    Find and remove metadata messages that reference incomplete files.

    Returns the number of partial uploads cleaned up.
    """
    if not telegram._channel_id:
        raise ValueError("No channel set")

    logger.info("Cleaning up partial uploads...")

    index = await telegram.get_index()
    cleaned = 0

    for file_id, msg_id in list(index.files.items()):
        try:
            metadata = await telegram.get_metadata(msg_id)
            if not metadata.is_complete():
                logger.info(
                    f"Removing incomplete file: {metadata.name} "
                    f"({len(metadata.chunks)}/{metadata.chunk_count} chunks)"
                )
                await telegram.delete_file(file_id)
                cleaned += 1
        except Exception as e:
            logger.warning(f"Could not check file {file_id}: {e}")

    logger.info(f"Cleaned up {cleaned} partial uploads")
    return cleaned
