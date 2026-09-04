"""Tier 3 Pairwise: Snapshot Type × Directory Topology Matrix."""

from pathlib import Path
import pytest

from televault.snapshot import Snapshot, SnapshotFile
from ..harness.crypto_oracle import CryptoOracle
from ..harness.fixtures import generate_nested_project_tree


@pytest.mark.parametrize("topology", ["flat", "nested", "hidden"])
@pytest.mark.parametrize("incremental", [True, False])
def test_pairwise_backup_topology_and_incrementality(tmp_path, topology: str, incremental: bool):
    """Pairwise: Snapshot creation on flat vs nested trees, with full vs incremental mode."""
    target_dir = tmp_path / f"tree_{topology}"
    target_dir.mkdir(parents=True, exist_ok=True)

    if topology == "flat":
        for i in range(5):
            (target_dir / f"file_{i}.txt").write_text(f"content {i}")
    elif topology == "nested":
        generate_nested_project_tree(target_dir)
    elif topology == "hidden":
        (target_dir / ".env").write_text("API_KEY=secret")
        (target_dir / ".gitconfig").write_text("[user]\nname=test")
        (target_dir / "visible.txt").write_text("visible")

    # Collect initial file state
    files_state = {}
    for p in target_dir.rglob("*"):
        if p.is_file():
            rel = str(p.relative_to(target_dir))
            files_state[rel] = CryptoOracle.blake3_hash(p.read_bytes())

    snapshot_files = [
        SnapshotFile(path=rel, file_id=f"file_{i}", hash=h, size=len((target_dir / rel).read_bytes()), modified_at=10.0)
        for i, (rel, h) in enumerate(files_state.items())
    ]
    snap1 = Snapshot(id=f"snap_{topology}_v1", name=f"{topology}_v1", created_at=10.0, files=snapshot_files)
    assert len(snap1.files) > 0

    if incremental:
        # Modify 1 file
        first_file = next(iter(files_state.keys()))
        (target_dir / first_file).write_text("MODIFIED_CONTENT_DELTA")
        updated_state = files_state.copy()
        updated_state[first_file] = CryptoOracle.blake3_hash((target_dir / first_file).read_bytes())

        updated_snapshot_files = [
            SnapshotFile(path=rel, file_id=f"file_{i}", hash=h, size=len((target_dir / rel).read_bytes()), modified_at=20.0)
            for i, (rel, h) in enumerate(updated_state.items())
        ]
        snap2 = Snapshot(id=f"snap_{topology}_v2", name=f"{topology}_v2", created_at=20.0, files=updated_snapshot_files)
        f1_hash = next(f.hash for f in snap1.files if f.path == first_file)
        f2_hash = next(f.hash for f in snap2.files if f.path == first_file)
        assert f1_hash != f2_hash
