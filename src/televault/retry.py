"""Retry logic with exponential backoff for TeleVault operations."""

import asyncio
import functools
import inspect
import logging
import random
from collections.abc import Callable
from typing import Any, TypeVar

logger = logging.getLogger("televault")

T = TypeVar("T")

# Default retryable exceptions from Telethon
RETRYABLE_EXCEPTIONS = (
    ConnectionError,
    TimeoutError,
    OSError,
)

# Will be extended with Telethon errors after import
_telethon_errors: tuple[type, ...] = ()


def _get_telethon_errors() -> tuple[type, ...]:
    """Lazy-load Telethon error types."""
    global _telethon_errors
    if not _telethon_errors:
        try:
            from telethon.errors import (
                FloodWaitError,
                NetworkMigrateError,
                PhoneMigrateError,
                ServerError,
            )

            _telethon_errors = (FloodWaitError, NetworkMigrateError, PhoneMigrateError, ServerError)
        except ImportError:
            _telethon_errors = ()
    return _telethon_errors


def get_retryable_exceptions() -> tuple[type, ...]:
    """Return tuple of exceptions that should trigger a retry."""
    return RETRYABLE_EXCEPTIONS + _get_telethon_errors()


def is_retryable(exc: Exception) -> bool:
    """Check if an exception is retryable."""
    retryable = get_retryable_exceptions()
    if isinstance(exc, retryable):
        return True

    # FloodWaitError is always retryable (handled separately with its own delay)
    try:
        from telethon.errors import FloodWaitError

        if isinstance(exc, FloodWaitError):
            return True
    except ImportError:
        pass

    # Some connection-related errors are retryable
    exc_str = str(exc).lower()
    retryable_patterns = [
        "connection reset",
        "connection refused",
        "timed out",
        "timeout",
        "network",
        "temporary failure",
        "server error",
    ]
    return any(pattern in exc_str for pattern in retryable_patterns)


async def retry_async(
    fn: Callable[..., Any],
    *,
    max_retries: int = 3,
    base_delay: float = 1.0,
    max_delay: float = 60.0,
    jitter: bool = True,
    on_retry: Callable[[int, Exception, float], Any] | None = None,
) -> Any:
    """
    Retry an async function with exponential backoff.

    Args:
        fn: Async callable to retry (called with no arguments)
        max_retries: Maximum number of retry attempts (0 = no retries)
        base_delay: Base delay in seconds (doubled each retry)
        max_delay: Maximum delay between retries
        jitter: Add random jitter to delay
        on_retry: Optional callback(attempt, exception, delay) called before each retry

    Returns:
        Result of fn()

    Raises:
        Last exception if all retries exhausted
    """
    last_exc: Exception | None = None

    for attempt in range(max_retries + 1):
        try:
            if inspect.iscoroutinefunction(fn):
                return await fn()
            else:
                return fn()
        except Exception as exc:
            last_exc = exc

            if attempt >= max_retries:
                logger.error(f"Operation failed after {max_retries + 1} attempts: {exc}")
                raise

            # Handle FloodWaitError: always retry with appropriate delay
            try:
                from telethon.errors import FloodWaitError

                if isinstance(exc, FloodWaitError):
                    delay = max(exc.seconds + 1, base_delay)
                    if exc.seconds > 300:
                        logger.warning(
                            f"FloodWaitError requires {exc.seconds}s wait "
                            f"(attempt {attempt + 1}/{max_retries})"
                        )
                    # Limit retries for long waits
                    if exc.seconds > 300 and attempt >= 2:
                        logger.error(
                            f"FloodWaitError >300s after {attempt + 1} attempts, giving up"
                        )
                        raise
                    await asyncio.sleep(delay)
                    if on_retry:
                        on_retry(attempt, exc, delay)
                    continue
            except ImportError:
                pass

            if not is_retryable(exc):
                logger.error(f"Non-retryable error: {exc}")
                raise

            # Calculate delay with exponential backoff
            delay = min(base_delay * (2**attempt), max_delay)
            if jitter:
                delay = delay * (0.5 + random.random())

            logger.warning(f"Retry {attempt + 1}/{max_retries} after {delay:.1f}s: {exc}")

            if on_retry:
                on_retry(attempt, exc, delay)

            await asyncio.sleep(delay)

    # Should not reach here, but just in case
    if last_exc:
        raise last_exc
    raise RuntimeError("Unexpected retry state")


def with_retry(
    max_retries: int = 3,
    base_delay: float = 1.0,
    max_delay: float = 60.0,
) -> Callable:
    """
    Decorator that adds retry logic to an async method.

    Usage:
        @with_retry(max_retries=3, base_delay=1.0)
        async def upload_chunk(self, ...):
            ...
    """

    def decorator(func: Callable) -> Callable:
        @functools.wraps(func)
        async def wrapper(*args: Any, **kwargs: Any) -> Any:
            last_exc: Exception | None = None

            # Extract config from self if available
            retry_max = max_retries
            retry_base = base_delay
            if args and hasattr(args[0], "config"):
                config = args[0].config
                if hasattr(config, "max_retries"):
                    retry_max = config.max_retries
                if hasattr(config, "retry_delay"):
                    retry_base = config.retry_delay

            for attempt in range(retry_max + 1):
                try:
                    return await func(*args, **kwargs)
                except Exception as exc:
                    last_exc = exc

                    if attempt >= retry_max:
                        raise

                    # Handle FloodWaitError specially
                    try:
                        from telethon.errors import FloodWaitError

                        if isinstance(exc, FloodWaitError):
                            delay = max(exc.seconds + 1, retry_base)
                            if exc.seconds > 300:
                                logger.warning(
                                    f"{func.__name__}: FloodWaitError {exc.seconds}s "
                                    f"(attempt {attempt + 1}/{retry_max})"
                                )
                                # Limit retries for long waits
                                if attempt >= 2:
                                    raise
                            await asyncio.sleep(delay)
                            continue
                    except ImportError:
                        pass

                    if not is_retryable(exc):
                        raise

                    delay = min(retry_base * (2**attempt), max_delay)
                    delay = delay * (0.5 + random.random())

                    logger.warning(
                        f"{func.__name__}: retry {attempt + 1}/{retry_max} "
                        f"after {delay:.1f}s: {exc}"
                    )

                    await asyncio.sleep(delay)

            if last_exc:
                raise last_exc
            raise RuntimeError("Unexpected retry state")

        return wrapper

    return decorator
