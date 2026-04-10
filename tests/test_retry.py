"""Tests for retry module."""

import asyncio
import pytest

from televault.retry import is_retryable, retry_async


class TestIsRetryable:
    def test_connection_error_is_retryable(self):
        assert is_retryable(ConnectionError("lost connection"))

    def test_timeout_error_is_retryable(self):
        assert is_retryable(TimeoutError("timed out"))

    def test_os_error_is_retryable(self):
        assert is_retryable(OSError("network error"))

    def test_value_error_is_not_retryable(self):
        assert not is_retryable(ValueError("bad input"))

    def test_type_error_is_not_retryable(self):
        assert not is_retryable(TypeError("wrong type"))

    def test_string_patterns_are_retryable(self):
        assert is_retryable(Exception("Connection reset by peer"))
        assert is_retryable(Exception("Timed out after 30s"))
        assert is_retryable(Exception("Network is unreachable"))

    def test_non_retryable_string(self):
        assert not is_retryable(Exception("Invalid authentication"))


@pytest.mark.asyncio
async def test_retry_async_success():
    call_count = 0

    async def success_fn():
        nonlocal call_count
        call_count += 1
        return "ok"

    result = await retry_async(success_fn, max_retries=3)
    assert result == "ok"
    assert call_count == 1


@pytest.mark.asyncio
async def test_retry_async_eventual_success():
    call_count = 0

    async def flaky_fn():
        nonlocal call_count
        call_count += 1
        if call_count < 3:
            raise ConnectionError("temporary failure")
        return "ok"

    result = await retry_async(flaky_fn, max_retries=3, base_delay=0.01)
    assert result == "ok"
    assert call_count == 3


@pytest.mark.asyncio
async def test_retry_async_exhausted():
    call_count = 0

    async def always_fail():
        nonlocal call_count
        call_count += 1
        raise ConnectionError("persistent failure")

    with pytest.raises(ConnectionError):
        await retry_async(always_fail, max_retries=2, base_delay=0.01)

    assert call_count == 3  # 1 initial + 2 retries


@pytest.mark.asyncio
async def test_retry_async_non_retryable():
    call_count = 0

    async def value_error_fn():
        nonlocal call_count
        call_count += 1
        raise ValueError("non-retryable")

    with pytest.raises(ValueError):
        await retry_async(value_error_fn, max_retries=3, base_delay=0.01)

    assert call_count == 1  # Should not retry
