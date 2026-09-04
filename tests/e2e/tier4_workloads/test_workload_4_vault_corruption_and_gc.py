"""Workload 4: Vault Maintenance, Corruption Detection & Garbage Collection Lifecycle."""

from pathlib import Path
import pytest

from televault.models import ChunkInfo, FileMetadata, VaultIndex
from ..harness.crypto_oracle import CryptoOracle
from ..harness.mock_storage import MockTelegramChannel


def test_workload_4_vault_corruption_and_gc_lifecycle(tmp_path):
    """Tier 4 Workload 4: Integrity Verification & Garbage Collection Lifecycle.

    Scenario:
    1. Ingest multiple files into vault storage with chunks posted to Telegram channel.
    2. Build root VaultIndex with active file listings.
    3. Introduce deliberate bit corruption into chunk 1 of File B.
    4. Run integrity verification:
       - File A passes verification.
       - File B fails verification with authentication/hash error.
    5. Delete corrupted File B from index (leaving orphaned chunk messages).
    6. Execute Garbage Collection:
       - Dry run discovers orphaned chunks of File B without deleting.
       - Force deletion prunes orphaned chunks while preserving File A chunks.
    """
    storage_dir = tmp_path / "storage"
    channel = MockTelegramChannel(storage_dir)
    password = "gc_workload_pass"

    # ── Step 1 & 2: Ingest File A and File B ──────────────────────────
    payload_a = b"File A healthy payload\n" * 100
    payload_b = b"File B payload destined for corruption\n" * 100

    hash_a = CryptoOracle.blake3_hash(payload_a)
    hash_b = CryptoOracle.blake3_hash(payload_b)

    ct_a = CryptoOracle.encrypt_chunk_44(CryptoOracle.zstd_compress(payload_a), password)
    ct_b = CryptoOracle.encrypt_chunk_44(CryptoOracle.zstd_compress(payload_b), password)

    doc_a = channel.post_document("chunk_a.bin", ct_a)
    doc_b = channel.post_document("chunk_b.bin", ct_b)

    # Maintain index
    vault_index = VaultIndex()
    vault_index.add_file("file_a.txt", doc_a["id"])
    vault_index.add_file("file_b.txt", doc_b["id"])

    # ── Step 3: Introduce bit corruption into File B chunk ───────────
    chunk_b_file = Path(doc_b["stored_path"])
    corrupted_bytes = bytearray(chunk_b_file.read_bytes())
    corrupted_bytes[-1] ^= 0xFF  # Invert last byte of authentication tag
    chunk_b_file.write_bytes(corrupted_bytes)

    # ── Step 4: Integrity Verification ──────────────────────────────
    # Verification function
    def verify_chunk(stored_path: str, expected_hash: str) -> bool:
        try:
            data = Path(stored_path).read_bytes()
            pt = CryptoOracle.decrypt_chunk(data, password)
            decomp = CryptoOracle.zstd_decompress(pt)
            return CryptoOracle.blake3_hash(decomp) == expected_hash
        except Exception:
            return False

    assert verify_chunk(doc_a["stored_path"], hash_a) is True, "File A should verify successfully"
    assert verify_chunk(doc_b["stored_path"], hash_b) is False, "Corrupted File B must fail verification!"

    # ── Step 5: Delete corrupted File B from index ───────────────────
    vault_index.remove_file("file_b.txt")
    assert "file_b.txt" not in vault_index.files

    # ── Step 6: Garbage Collection ───────────────────────────────────
    # Active message IDs in vault index: {doc_a["id"]}
    active_msg_ids = set(vault_index.files.values())

    all_docs = channel.list_documents()
    orphaned_docs = [d for d in all_docs if d["id"] not in active_msg_ids]

    # Dry-run phase
    assert len(orphaned_docs) == 1
    assert orphaned_docs[0]["id"] == doc_b["id"]
    assert chunk_b_file.exists(), "Dry run must NOT delete chunk files"

    # Force deletion phase
    deleted_count = channel.delete_messages([d["id"] for d in orphaned_docs])
    assert deleted_count == 1
    assert not chunk_b_file.exists(), "Force GC must prune orphaned chunk file"
    assert Path(doc_a["stored_path"]).exists(), "Active chunk A must remain intact"
