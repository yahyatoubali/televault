"""Tier 3 Pairwise: Concurrency Workers × Cache Capacity Matrix."""

from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
import pytest


@pytest.mark.parametrize("workers", [1, 4, 8])
@pytest.mark.parametrize("cache_capacity", [2, 50])
def test_pairwise_concurrent_cache_access(workers: int, cache_capacity: int):
    """Pairwise: Multi-threaded worker pool interacting with chunk cache."""
    cache = OrderedDict()
    import threading
    lock = threading.Lock()

    def worker_task(thread_id: int):
        for i in range(20):
            key = f"chunk_{thread_id}_{i % 5}"
            val = f"data_{thread_id}_{i}".encode("ascii")
            with lock:
                cache[key] = val
                cache.move_to_end(key)
                if len(cache) > cache_capacity:
                    cache.popitem(last=False)

    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = [pool.submit(worker_task, tid) for tid in range(workers)]
        for f in futures:
            f.result()

    with lock:
        assert len(cache) <= cache_capacity
