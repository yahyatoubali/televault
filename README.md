# TeleVault

Unlimited cloud storage using your **own** Telegram account. No local database — everything lives in a private Telegram channel, encrypted on your machine before upload.

## What's New

### v2.2.0 — Data Safety & Backups

- **Backup Snapshots** — Create, restore, list, prune, and verify directory backups
- **Retry Logic** — Exponential backoff on all Telegram operations (3 retries, FloodWait handling)
- **Atomic Index Updates** — Version-based concurrency control prevents data loss from race conditions
- **Parallel Downloads** — Configurable concurrency using existing `parallel_downloads` setting
- **Upload Cleanup** — Failed uploads now automatically clean up orphaned messages
- **Original Hash Verification** — Chunks store pre-processing hash for decrypt verification
- **Progress File Integrity** — CRC32 checksums on `.progress` files detect corruption
- **Garbage Collection** — `televault gc` finds and removes orphaned messages
- **File Verification** — `televault verify <file>` re-downloads and checks all chunk hashes
- **Verbose/Debug Logging** — `televault -v` and `televault --debug` flags
- **TUI Connection Pooling** — Persistent connection instead of reconnecting per operation

### v2.1.0

- Resumable transfers with `--resume` flag
- Enhanced TUI with progress bars, confirmation dialogs, file type icons
- Fixed login flow with 2FA support

---

## Installation

```bash
pip install televault
```

Python 3.11+ is required.

---

## Quick Start

```bash
# 1) Set up your Telegram API credentials (one-time setup)
export TELEGRAM_API_ID=your_api_id
export TELEGRAM_API_HASH=your_api_hash

# 2) Login with your Telegram account
televault login

# 3) Set up storage channel (interactive)
televault setup

# 4) Upload a file
TELEVAULT_PASSWORD="strong-password" televault push /path/to/file

# 5) List files
televault ls

# 6) Download a file
TELEVAULT_PASSWORD="strong-password" televault pull <file_id_or_name>
```

---

## CLI Commands

| Command | Description |
|---------|-------------|
| `televault login` | Authenticate with Telegram |
| `televault setup` | Set up storage channel |
| `televault push <file>` | Upload a file |
| `televault pull <file>` | Download a file |
| `televault ls` | List all files |
| `televault search <query>` | Search files by name |
| `televault rm <file>` | Delete a file |
| `televault status` | Show vault statistics |
| `televault whoami` | Show current account |
| `televault verify <file>` | Verify file integrity |
| `televault gc` | Garbage collect orphaned messages |
| `televault backup create <dir>` | Create a backup snapshot |
| `televault backup restore <id>` | Restore from a snapshot |
| `televault backup list` | List all snapshots |
| `televault backup prune` | Prune old snapshots |
| `televault backup verify <id>` | Verify a snapshot |
| `televault tui` | Launch interactive TUI |
| `televault logout` | Clear session |

### Upload Options

```bash
# Upload with encryption (default)
televault push myfile.txt --password mypassword

# Upload without encryption
televault push myfile.txt --no-encrypt

# Upload directory recursively
televault push myfolder/ -r

# Resume interrupted upload
televault push largefile.zip --resume
```

### Download Options

```bash
# Download with decryption
televault pull myfile.txt --password mypassword

# Download to specific path
televault pull myfile.txt --output /path/to/save

# Resume interrupted download
televault pull largefile.zip --resume
```

---

## Backup & Restore

TeleVault supports git-like backup snapshots of directories:

```bash
# Create a full backup
televault backup create /important/data --name "daily-backup"

# Create an incremental backup (only changed files)
televault backup create /important/data --name "daily" --incremental

# Dry run (show what would be backed up)
televault backup create /important/data --dry-run

# List all snapshots
televault backup list

# Restore from a snapshot
televault backup restore <snapshot_id> --output /restore/path

# Restore specific files
televault backup restore <snapshot_id> --output /restore/path --files docs/readme.md

# Verify snapshot integrity
televault backup verify <snapshot_id>

# Prune old snapshots (keep last 7 daily, 4 weekly, 6 monthly)
televault backup prune --keep-daily 7 --keep-weekly 4 --keep-monthly 6

# Dry-run prune
televault backup prune --dry-run

# Delete a specific snapshot
televault backup delete <snapshot_id>
```

### How It Works

- Each snapshot stores file metadata (path, hash, size, file ID)
- Only changed files are uploaded in incremental backups
- Snapshots reference files already in the vault (deduplication)
- Restore downloads files from the vault using their file IDs
- Pruning respects retention policies (daily/weekly/monthly)

---

## Data Safety

TeleVault is designed for people who care about their data:

- **Retry Logic** — All Telegram operations retry 3x with exponential backoff
- **Atomic Index Updates** — Version-based concurrency control prevents data races
- **Upload Cleanup** — Failed uploads automatically delete orphaned messages
- **Hash Verification** — Every chunk is verified with BLAKE3 on download
- **Original Hash** — Separate hash for pre-encryption data catches wrong-password errors
- **Progress Integrity** — CRC32 checksums on resume files detect corruption
- **Parallel Downloads** — Configurable concurrency for faster downloads
- **Garbage Collection** — Find and remove orphaned messages

---

## Interactive TUI

Launch the Terminal User Interface for visual file management:

```bash
televault tui
```

### TUI Features

- **File Browser** — Browse all files with icons, sizes, and encryption status
- **Progress Bars** — Real-time upload/download progress with chunk counts
- **Search** — Live search through your files
- **Confirmations** — Safe delete with confirmation dialogs
- **Login Flow** — Complete login flow with 2FA support

### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `q` | Quit |
| `u` | Upload file |
| `d` | Download file |
| `/` | Search |
| `Delete` | Delete file (with confirmation) |
| `l` | Login |
| `r` | Refresh |

---

## Resumable Transfers

Large file transfers can be interrupted. Resume them with:

```bash
# Resume upload
televault push largefile.zip --resume

# Resume download
televault pull largefile.zip --resume
```

Progress is tracked per-chunk with CRC32 integrity checks.

---

## Security Model

TeleVault encrypts data **before** it leaves your machine:

- **AES-256-GCM** encryption with password-derived keys
- **scrypt** key derivation (memory-hard)
- **BLAKE3** hashing for chunk verification
- **zstd** compression (optional)

Your password never leaves your machine. Telegram only sees encrypted blobs and JSON metadata.

> **Important:** If you lose your password and have encryption enabled, there is no recovery.

---

## Configuration

Config file location: `~/.config/televault/config.json`

```json
{
  "channel_id": -1003652003243,
  "chunk_size": 104857600,
  "compression": true,
  "encryption": true,
  "parallel_uploads": 3,
  "parallel_downloads": 5,
  "max_retries": 3,
  "retry_delay": 1.0
}
```

Logging output: `~/.local/share/televault/televault.log`

Enable verbose/debug logging:
```bash
televault -v ls      # Info level
televault --debug ls # Debug level
```

---

## Development

```bash
git clone https://github.com/YahyaToubali/televault.git
cd televault
python -m venv .venv
source .venv/bin/activate
pip install -e ".[dev]"

# Run tests
pytest

# Run linter
ruff check src/
```

---

## Requirements

- Python 3.11+
- Telegram account
- API credentials from [my.telegram.org](https://my.telegram.org)

---

## License

MIT License - See [LICENSE](LICENSE) for details.

**Author:** Yahya Toubali · [@yahyatoubali](https://github.com/YahyaToubali)