# The TeleVault Engine (C++23 Native)

TeleVault uses a private Telegram channel as an encrypted, distributed object store. All state lives on Telegram as pinned index messages, metadata nodes, and chunk reply chains.

## Message Topology

```
                  Private Channel
                 =================
                 | Pinned: VaultIndex | ─── files: { "file_id": metadata_msg_id, ... }
                 =================
                         │
           ┌─────────────┴─────────────┐
           ▼                           ▼
    FileMetadata 1              FileMetadata 2
    (JSON text msg)             (JSON text msg)
           │                           │
     ┌─────┼─────┐               ┌─────┴─────┐
     ▼     ▼     ▼               ▼           ▼
  Chunk0 Chunk1 Chunk2        Chunk0      Chunk1
  (file) (file) (file)        (file)      (file)

   ───────────────────────────────────────────────────
   ISOLATED SNAPSHOT SYSTEM (Separate Message Tree)
   ───────────────────────────────────────────────────
   SnapshotIndex Message ─── snapshots: { "snap_id": snap_msg_id, ... }
         │
         ├── Snapshot 1 (JSON text msg: list of file entries and relative paths)
         └── Snapshot 2 (JSON text msg)
```

### Safety Invariant: Index Decoupling
The pinned message in the channel is **strictly reserved** for the primary `VaultIndex` (file listing). Snapshots are stored in their own message tree tracked by `snapshot_index_msg_id` in `config.json`. This architecture ensures that snapshot operations (creation, restoration, pruning) never interfere with or overwrite the user's file inventory.

---

## The Upload Pipeline

```
Local File
    │
    ▼
Chunking (100 MB default; 32 MB low-resource)
    │
    ▼
Compute Blake3 Plaintext Hash (ci.original_hash)
    │
    ▼
Zstandard Compression (level 3) -- auto-bypassed for media files
    │
    ▼
AES-256-GCM Encryption (Argon2id / PBKDF2 derived key, random nonce)
    │
    ▼
Compute Blake3 Ciphertext Hash (ci.hash)
    │
    ▼
Transmit via TDLib MTProto as Document replying to FileMetadata
```

---

## Download Integrity & Atomic Swapping

1. Pre-allocates or streams to a temporary sibling file (`target.partial.XXXXXX`).
2. Validates chunk ciphertext hash against `ci.hash`.
3. Decrypts and decompresses payload.
4. Validates decompressed plaintext against `ci.original_hash` to detect wrong password or corrupted decryption.
5. Verifies assembled full file against `FileMetadata.hash`.
6. Executes atomic filesystem rename to ensure no partial or corrupt files ever replace existing data.

---

## Sub-Second Previews (`tvt preview`)

Instead of downloading whole multi-gigabyte files to preview content:
- `tvt preview` queries only Chunk 0 (`index == 0`).
- Decrypts and decompresses chunk 0 in-flight.
- Classifies MIME type and renders the leading lines with syntax formatting in < 1 second.
