"""Shared utility functions for TeleVault."""


def format_size(size: int) -> str:
    """Format bytes as human readable string.

    Args:
        size: Size in bytes

    Returns:
        Human-readable size string (e.g., "1.5 MB")
    """
    if size < 0:
        return "0 B"
    size_float: float = size
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if size_float < 1024:
            return f"{size_float:.1f} {unit}"
        size_float /= 1024
    return f"{size_float:.1f} PB"


def format_speed(bytes_per_sec: float) -> str:
    """Format transfer speed as human readable string.

    Args:
        bytes_per_sec: Bytes per second

    Returns:
        Human-readable speed string (e.g., "2.3 MB/s")
    """
    if bytes_per_sec <= 0:
        return ""
    for unit in ["B/s", "KB/s", "MB/s", "GB/s"]:
        if bytes_per_sec < 1024:
            return f"{bytes_per_sec:.1f} {unit}"
        bytes_per_sec /= 1024
    return f"{bytes_per_sec:.1f} TB/s"
