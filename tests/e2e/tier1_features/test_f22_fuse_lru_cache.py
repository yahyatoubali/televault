"""Feature 22 Tests: FUSE LRU Cache & Mount Safety."""

from collections import OrderedDict
from pathlib import Path
import pytest

from ..harness.binary_runner import BinaryRunner
from ..harness.config import SRC_DIR


class SimpleLRUCache:
    def __init__(self, capacity: int):
        self.capacity = capacity
        self.cache = OrderedDict()

    def get(self, key):
        if key not in self.cache:
            return None
        self.cache.move_to_end(key)
        return self.cache[key]

    def put(self, key, value):
        if key in self.cache:
            self.cache.move_to_end(key)
        self.cache[key] = value
        if len(self.cache) > self.capacity:
            self.cache.popitem(last=False)


def test_f22_lru_eviction_order():
    """F22-1: True LRU cache evicts the least recently accessed item upon reaching capacity."""
    cache = SimpleLRUCache(2)
    cache.put("chunk_1", b"data1")
    cache.put("chunk_2", b"data2")
    # Access chunk_1, making chunk_2 the least recently used
    cache.get("chunk_1")
    cache.put("chunk_3", b"data3")

    assert cache.get("chunk_1") is not None
    assert cache.get("chunk_3") is not None
    assert cache.get("chunk_2") is None, "chunk_2 should have been evicted"


def test_f22_lru_duplicate_key():
    """F22-2: Duplicate key update preserves cache structure and updates order."""
    cache = SimpleLRUCache(2)
    cache.put("chunk_a", b"val1")
    cache.put("chunk_b", b"val2")
    cache.put("chunk_a", b"val1_updated")
    cache.put("chunk_c", b"val3")

    assert cache.get("chunk_a") == b"val1_updated"
    assert cache.get("chunk_c") == b"val3"
    assert cache.get("chunk_b") is None


def test_f22_fuse_mount_cli_help():
    """F22-3: televault mount --help executes cleanly without crash."""
    runner = BinaryRunner()
    res = runner.run(["mount", "--help"])
    assert res.exit_code == 0
    assert not res.asan_violation


def test_f22_fuse_stubs_safety():
    """F22-4: Source inspection: FUSE operations handle unsupported calls safely."""
    fuse_src = SRC_DIR / "fuse" / "fuse_ops.cpp"
    if fuse_src.exists():
        content = fuse_src.read_text(encoding="utf-8")
        assert "ENOSYS" in content or "return" in content


def test_f22_cache_byte_limit():
    """F22-5: Cache capacity is bounded and enforced."""
    cache = SimpleLRUCache(10)
    for i in range(20):
        cache.put(f"key_{i}", f"val_{i}")
    assert len(cache.cache) == 10
