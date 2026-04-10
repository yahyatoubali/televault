"""Tests for TeleVault FUSE filesystem (unit tests without FUSE mount)."""

import stat

import pytest

from televault.fuse import FUSE_AVAILABLE, LRUCache


class TestLRUCache:
    def test_put_and_get(self):
        cache = LRUCache(max_size_mb=1)
        cache.put("key1", b"value1")
        assert cache.get("key1") == b"value1"

    def test_get_missing(self):
        cache = LRUCache(max_size_mb=1)
        assert cache.get("missing") is None

    def test_eviction(self):
        cache = LRUCache(max_size_mb=1)  # 1MB = 1048576 bytes
        cache.put("a", b"x" * 500000)
        cache.put("b", b"y" * 500000)
        # Total ~1MB, both should fit
        assert cache.get("a") is not None
        assert cache.get("b") is not None

        # Adding a third should evict oldest
        cache.put("c", b"z" * 500000)
        assert cache.get("a") is None  # evicted

    def test_size_tracking(self):
        cache = LRUCache(max_size_mb=1)
        cache.put("key1", b"x" * 1024)
        assert cache.size_mb > 0
        assert cache.size_mb < 0.01  # ~1KB

    def test_overwrite_updates_value(self):
        cache = LRUCache(max_size_mb=1)
        cache.put("key1", b"old_value")
        cache.put("key1", b"new_value")
        assert cache.get("key1") == b"new_value"

    def test_clear(self):
        cache = LRUCache(max_size_mb=1)
        cache.put("key1", b"val1")
        cache.put("key2", b"val2")
        cache.clear()
        assert cache.get("key1") is None
        assert cache.size_mb == 0

    def test_has(self):
        cache = LRUCache(max_size_mb=1)
        cache.put("key1", b"val")
        assert cache.has("key1")
        assert not cache.has("missing")

    def test_lru_ordering(self):
        cache = LRUCache(max_size_mb=1)
        # Fill cache, then access "a" to make it recent
        cache.put("a", b"x" * 400000)
        cache.put("b", b"y" * 400000)
        _ = cache.get("a")  # access "a" to make it recent
        cache.put("c", b"z" * 400000)  # should evict "b", not "a"
        assert cache.get("a") is not None
        assert cache.get("b") is None  # evicted


class TestFuseAvailability:
    def test_fuse_module_imports(self):
        if FUSE_AVAILABLE:
            from televault.fuse import TeleVaultFuse

            assert TeleVaultFuse is not None
        else:
            with pytest.raises(ImportError):
                from fuse import FUSE
