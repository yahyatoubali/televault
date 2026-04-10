"""Structured logging for TeleVault."""

import logging
import sys
from logging.handlers import RotatingFileHandler

from .config import get_data_dir


def setup_logging(level: str = "WARNING", log_file: bool = True) -> logging.Logger:
    """
    Configure TeleVault logging.

    Args:
        level: Log level (DEBUG, INFO, WARNING, ERROR, CRITICAL)
        log_file: Whether to write logs to file

    Returns:
        Configured logger instance
    """
    logger = logging.getLogger("televault")
    logger.setLevel(getattr(logging, level.upper(), logging.WARNING))

    # Clear existing handlers
    logger.handlers.clear()

    # Console handler (stderr)
    console_handler = logging.StreamHandler(sys.stderr)
    console_handler.setLevel(getattr(logging, level.upper(), logging.WARNING))
    console_formatter = logging.Formatter("%(levelname)s: %(message)s")
    console_handler.setFormatter(console_formatter)
    logger.addHandler(console_handler)

    # File handler
    if log_file:
        log_dir = get_data_dir()
        log_dir.mkdir(parents=True, exist_ok=True)
        log_path = log_dir / "televault.log"

        file_handler = RotatingFileHandler(log_path, maxBytes=10 * 1024 * 1024, backupCount=3)
        file_handler.setLevel(logging.DEBUG)
        file_formatter = logging.Formatter(
            "%(asctime)s [%(levelname)s] %(name)s:%(funcName)s:%(lineno)d - %(message)s"
        )
        file_handler.setFormatter(file_formatter)
        logger.addHandler(file_handler)

    return logger


def get_logger(name: str | None = None) -> logging.Logger:
    """Get a TeleVault logger, optionally with a sub-name."""
    if name:
        return logging.getLogger(f"televault.{name}")
    return logging.getLogger("televault")
