# TeleVault

Unlimited cloud storage using your **own** Telegram account. No local database — everything lives in a private Telegram channel, encrypted on your machine before upload.

## What's New in v2.1.0

- **Resumable Transfers** - Interrupted uploads/downloads can be resumed with `--resume` flag
- **Enhanced TUI** - New confirmation dialogs, progress bars, and file type icons
- **Better Error Handling** - Improved cleanup on failures, directory auto-creation
- **Fixed Login Flow** - Proper 2FA password support in TUI

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

## Interactive TUI

Launch the Terminal User Interface for visual file management:

```bash
televault tui
```

### TUI Features

- **File Browser** -Browse all files with icons, sizes, and encryption status
- **Progress Bars** - Real-time upload/download progress with chunk counts
- **Search** - Live search through your files
- **Confirmations** - Safe delete with confirmation dialogs
- **Login Flow** - Complete login flow with 2FA support

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

Progress is tracked per-chunk, so you can safely interrupt and resume later.

---

## Storage Channel Setup

### Interactive Mode (Recommended)

```bash
televault setup
```

Choose to create a new private channel or use an existing one.

### Non-Interactive Options

```bash
# Auto-create new channel
televault setup --auto-create

# Use existing channel
televault setup --channel-id -1001234567890
```

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
  "parallel_downloads": 5
}
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