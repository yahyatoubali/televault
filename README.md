# TeleVault

Unlimited cloud storage using your **own** Telegram account. No local database — everything lives in a private Telegram channel, encrypted on your machine before upload.

## What's New

### v2.5.0 — Preview System & CLI Restructure

- **`tvt` command** — Short, Unix-style CLI (`tvt push`, `tvt pull`, `tvt ls`, etc.)
- **Pipeable I/O** — `cat file | tvt push - --name file`, `tvt cat photo.jpg > photo.jpg`
- **`tvt cat`** — Stream file content to stdout for piping
- **`tvt preview`** — Terminal preview without full download (images, videos, text, hex)
- **`tvt stat`** — Quick vault statistics (rename of `status`)
- **`tvt find`** — Search files (alias for `search`)
- **`tvt completion`** — Generate shell completion scripts (bash, zsh, fish, PowerShell)
- **`--json` flag** — Pipeable JSON output on `ls`, `stat`, `info`, `find`
- **File ID cache** — Auto-caches file IDs for shell completion
- **Preview engine** — Downloads first chunk only, extracts metadata from headers (PNG/JPEG dimensions, MP4 format, WAV params, etc.)

### v2.4.0 — Auto Backup

- **Scheduled Backups** — Create, list, run, and delete backup schedules
- **systemd Timer Integration** — Install schedules as systemd timers on Linux
- **Cron Support** — Generate crontab entries for any Unix system
- **File Watcher** — Watch directories for changes and auto-upload
- **Exclude Patterns** — Skip .git, __pycache__, .DS_Store, etc.

### v2.3.0 — Virtual Drive (FUSE & WebDAV)

- **FUSE Mount** — Mount your vault as a local filesystem on Linux/macOS
- **WebDAV Server** — Access files over HTTP/WebDAV from any device
- **Local Cache** — Files are cached locally for fast reads
- **Read-Only Mode** — Mount vaults safely without write access

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

# Optional extras
pip install televault[fuse]       # FUSE mount support
pip install televault[webdav]     # WebDAV server support
pip install televault[preview]    # Image preview support (Pillow)
pip install televault[dev]        # Development tools
pip install -e ".[dev,fuse,webdav,preview]"  # Everything
```

Python 3.11+ is required.

---

## Quick Start

```bash
# 1) Set up your Telegram API credentials (one-time setup)
export TELEGRAM_API_ID=your_api_id
export TELEGRAM_API_HASH=your_api_hash

# 2) Login with your Telegram account
tvt login

# 3) Set up storage channel (interactive)
tvt setup

# 4) Upload a file
TELEVAULT_PASSWORD="strong-password" tvt push /path/to/file

# 5) List files
tvt ls

# 6) Download a file
TELEVAULT_PASSWORD="strong-password" tvt pull <file_id_or_name>

# 7) Stream a file to stdout
tvt cat photo.jpg > photo.jpg

# 8) Preview a file without full download
tvt preview photo.jpg

# 9) Install shell completion
tvt completion bash >> ~/.bashrc
```

---

## Shell Completion

```bash
# Bash
tvt completion bash >> ~/.bashrc

# Zsh
tvt completion zsh > ~/.zfunc/_tvt

# Fish
tvt completion fish > ~/.config/fish/completions/tvt.fish

# PowerShell
tvt completion powershell | Add-Content $PROFILE
```

---

## Pipeable I/O

TeleVault commands are designed to work with Unix pipes:

```bash
# Upload from stdin
echo "hello world" | tvt push - --name note.txt
cat config.json | tvt push - --name config.json
mysqldump mydb | tvt push - --name db-backup.sql

# Download to stdout
tvt cat config.json | jq '.database'
tvt pull video.mp4 -o - | mpv -

# JSON output for scripting
tvt ls --json | jq '.[].name'
tvt stat --json | jq '.file_count'
tvt find "backup" --json | jq '.[].size'
tvt info photo.jpg --json | jq '.hash'
```

---

## CLI Commands

| Command | Description |
|---------|-------------|
| `tvt push <file>` | Upload a file (use `-` for stdin) |
| `tvt pull <file>` | Download a file (use `-o -` for stdout) |
| `tvt ls` | List all files (`--json` for pipeable output) |
| `tvt find <query>` | Search files by name (`--json`) |
| `tvt rm <file>` | Delete a file |
| `tvt cat <file>` | Stream file content to stdout |
| `tvt preview <file>` | Show file preview without full download |
| `tvt info <file>` | Show detailed file info (`--json`) |
| `tvt stat` | Show vault statistics (`--json`) |
| `tvt verify <file>` | Verify file integrity |
| `tvt gc` | Garbage collect orphaned messages |
| `tvt backup create <dir>` | Create a backup snapshot |
| `tvt backup restore <id>` | Restore from a snapshot |
| `tvt backup list` | List all snapshots |
| `tvt backup prune` | Prune old snapshots |
| `tvt backup verify <id>` | Verify a snapshot |
| `tvt mount <dir>` | Mount vault as local filesystem (FUSE) |
| `tvt serve` | Start WebDAV server |
| `tvt schedule create <dir>` | Create a backup schedule |
| `tvt schedule list` | List backup schedules |
| `tvt schedule run <name>` | Run a schedule immediately |
| `tvt schedule install <name>` | Install schedule as systemd timer |
| `tvt watch --path <dir>` | Watch directory and auto-upload |
| `tvt login` | Authenticate with Telegram |
| `tvt setup` | Set up storage channel |
| `tvt tui` | Launch interactive TUI |
| `tvt completion <shell>` | Generate shell completion |
| `tvt logout` | Clear session |

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

## Virtual Drive

### FUSE Mount (Linux/macOS)

Mount your TeleVault as a local directory:

```bash
# Install FUSE support
pip install televault[fuse]

# Create mount point
mkdir -p ~/televault-drive

# Mount the vault
televault mount -m ~/televault-drive

# Read-only mount
televault mount -m ~/televault-drive --read-only

# Unmount (Ctrl+C or)
fusermount -u ~/televault-drive
```

Requirements:
- **Linux**: `sudo apt install fuse libfuse2`
- **macOS**: Install [macFUSE](https://macfuse.io/)

### WebDAV Server (All Platforms)

Access your vault over HTTP/WebDAV from any device:

```bash
# Install WebDAV support
pip install televault[webdav]

# Start server on default port 8080
televault serve

# Custom host and port
televault serve --host 0.0.0.0 --port 9090

# Read-only server
televault serve --read-only
```

Connect from any WebDAV client:
- **macOS Finder**: Go → Connect to Server → `http://localhost:8080/`
- **Windows**: Map Network Drive → `http://localhost:8080/`
- **Linux**: `davfs2` or file manager WebDAV support
- **Mobile**: Documents by Readdle, Solid Explorer, etc.

---

## Auto Backup

### Scheduled Backups

Create schedules that run automatically via systemd timers or cron:

```bash
# Create a daily backup schedule
televault schedule create /important/data --name "daily-docs" --interval daily

# Create an hourly incremental schedule
televault schedule create /important/data --name "hourly-sync" --interval hourly --incremental

# List all schedules
televault schedule list

# Run a schedule manually
televault schedule run daily-docs

# Install as systemd timer (Linux)
televault schedule install daily-docs

# Show systemd unit files without installing
televault schedule show-systemd daily-docs

# Uninstall systemd timer
televault schedule uninstall daily-docs

# Delete a schedule
televault schedule delete daily-docs
```

### File Watcher

Watch directories for changes and automatically upload new/modified files:

```bash
# Watch a directory
televault watch --path /important/docs

# Watch multiple directories
televault watch --path /docs --path /photos --path /projects

# Custom poll interval (default: 5 seconds)
televault watch --path /docs --interval 10

# Exclude custom patterns
televault watch --path /docs --exclude "*.tmp" --exclude "build/"
```

The watcher detects new files and file modifications, automatically uploading changes to your vault.

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
pip install televault[fuse]     # FUSE support (mount as filesystem)
pip install televault[webdav]    # WebDAV server support
pip install televault[dev]       # Development tools
pip install -e ".[dev,fuse,webdav]"  # Everything

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