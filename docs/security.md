# Security Protocol

TeleVault uses a zero-trust model: your plaintext never leaves your machine. Telegram only ever sees encrypted ciphertext.

## Encrypt-then-Upload Pipeline

```
┌──────────┐    ┌──────────┐    ┌──────────────┐    ┌────────────┐    ┌──────────┐
│ Original  │───▶│  Chunk   │───▶│  BLAKE3 Hash │───▶│  zstd      │───▶│  AES-256 │
│  File     │    │  (256MB) │    │  (original)  │    │  Compress  │    │  GCM     │
└──────────┘    └──────────┘    └──────────────┘    └────────────┘    └──────────┘
                                                                              │
                                                                              ▼
┌──────────┐    ┌──────────┐    ┌──────────────┐    ┌────────────┐    ┌──────────┐
│ Telegram  │◀───│ Upload   │◀───│ BLAKE3 Hash  │◀───│ Ciphertext │    │ Encrypted│
│ Channel   │    │ File Msg │    │ (encrypted)  │    │ + Tag      │    │ Chunk    │
└──────────┘    └──────────┘    └──────────────┘    └────────────┘    └──────────┘
```

Every stage is verified:

1. **Pre-processing hash** — BLAKE3 of the raw chunk before any transformation
2. **Post-processing hash** — BLAKE3 of the encrypted, compressed chunk
3. **File-level hash** — BLAKE3 of the entire reassembled file after download

## AES-256-GCM

Each chunk is encrypted independently with its own key material:

| Parameter | Value |
|---|---|
| Cipher | AES-256-GCM |
| Key Size | 256 bits (32 bytes) |
| Salt | 16 bytes (random, per chunk) |
| Nonce | 12 bytes (random, per chunk) |
| Auth Tag | 16 bytes (GCM) |
| Overhead | 44 bytes per chunk |

### Chunk Structure on Disk

```
[16-byte salt][12-byte nonce][ciphertext...][16-byte auth tag]
```

The salt and nonce are prepended to the ciphertext. During decryption, they are extracted from the first 28 bytes of the chunk data.

## Key Derivation: scrypt

```python
key = scrypt(
    password=user_password,
    salt=random_16_bytes,
    N=131072,      # 2^17
    r=8,
    p=1,
    dklen=32       # 256-bit key
)
```

The scrypt parameters are chosen to balance security and performance:

- **N = 2^17** — Memory-hard, requires ~1 MB RAM per derivation
- **r = 8** — Block size parameter
- **p = 1** — Parallelization factor

Each chunk gets its own random salt, so even identical plaintext chunks produce different ciphertext.

## BLAKE3 Integrity

BLAKE3 is used at three levels:

| Level | What | When |
|---|---|---|
| `original_hash` | Raw chunk data (pre-processing) | Before compression/encryption |
| `ChunkInfo.hash` | Encrypted chunk data (post-processing) | After compression/encryption |
| `FileMetadata.hash` | Entire file (all chunks concatenated) | After download completes |

### Why Two Hashes Per Chunk

The `original_hash` catches a subtle attack: a wrong password can produce ciphertext that passes GCM tag verification but decrypts to garbage. By verifying the pre-processing hash after decryption, TeleVault catches this case immediately:

```
decrypt → decompress → verify original_hash → FAIL → wrong password
```

The `ChunkInfo.hash` verifies the encrypted data hasn't been corrupted during upload or download.

## Zero-Trust Model

- **Password never leaves your machine** — it is used only for local scrypt key derivation
- **No server-side encryption** — Telegram stores only ciphertext
- **No key escrow** — there is no recovery mechanism. If you lose your password, your data is unrecoverable
- **No metadata leakage** — file names are stored in the VaultIndex (encrypted in the index message), not in Telegram message metadata
- **No local database** — all state is on Telegram, so there is no local file to steal

## Crash Safety

The upload and delete sequences are designed to be safe against crashes at any point:

### Upload Sequence

1. Upload metadata message → get `metadata_msg_id`
2. Upload all chunks in parallel
3. On failure: delete all uploaded chunks + metadata (cleanup)
4. Update metadata with chunk info
5. Save index (3 retries with backoff)

If the process crashes after step 2 but before step 5, the data exists on Telegram but is not in the index. Run `tvt gc --clean-partials` to clean up.

### Delete Sequence

1. Remove file from index first
2. Read metadata to collect chunk message IDs
3. Delete all messages (metadata + chunks)

If the process crashes after step 1, the file is no longer referenced. `tvt gc` finds and cleans the orphaned messages.

## Resumable Downloads

Progress is saved to a `.progress` file after each chunk, with CRC32 integrity checking:

```json
{
  "file_id": "abc123",
  "completed_chunks": [0, 1, 2],
  "total_chunks": 5,
  "checksum": "a1b2c3d4"
}
```

If the progress file is corrupted (CRC32 mismatch), the download starts fresh. Partial data (`.partial` file) is preserved across retries.
