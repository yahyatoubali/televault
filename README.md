# TeleVault

Unlimited cloud storage using Telegram MTProto. No local database — everything lives on Telegram.

## Features

- **MTProto Direct** — No bot API limits, 2GB file support
- **Zero Local DB** — Metadata stored on Telegram itself
- **Client-Side Encryption** — AES-256-GCM before upload
- **Smart Chunking** — Large files split automatically
- **Parallel Processing** — Faster uploads/downloads
- **Folder Support** — Preserve directory structure
- **Resume Capability** - Continue interrupted transfers
- **TUI + CLI** - Beautiful terminal interface

## Install

```bash
pip install televault
```

Or from source:

```bash
git clone https://github.com/YahyaToubali/televault
cd televault
pip install -e .".
```

## Quick Start

```bash
# First time: authenticate with Telegram
televault login

# Upload a file
televault push backup.tar.gz

# Upload a directory
televault push ~/Documents/ -r --no-encrypt

# List files
televault ls

# Download
televault pull backup.tar.gz

# Interactive TUI
televault
```

## How It Works

```
Your File
    ↓
[Compress] → zstd (optional, skips media)
    ↓
[Encrypt] → AES-256-GCM with Scrypt key derivation
    ↓
[Chunk] → Split into ≤2GB pieces
    ↓
[Upload] → MTProto to your private channel
    ↓
[Index] → Metadata stored as pinned message
```

### Channel Structure

```
📌 INDEX (pinned)
│   └── {"files": {"id1": msg_id, "id2": msg_id, ...}}
│
├── 📄 Metadata Message (JSON)
│   └── {"name": "file.zip", "size": 5GB, "chunks": [...]}
│
└── 📦 Chunk Messages (reply to metadata)
    └── file_id_001.chunk, file_id_002.chunk, ...
```

## Commands

| Command | Description |
|---------|-------------|
| `televault login` | Authenticate with Telegram |
| `televault logout` | Clear session |
| `televault push <file>` | Upload file (or directory with -r) |
| `televault pull <file>` | Download file |
| `televault ls` | List all files |
| `televault search <query>` | Search files by name |
| `televault rm <file>` | Delete file |
| `televault info <file>` | Show file details |
| `televault status` | Show vault status |
| `televault whoami` | Show current Telegram account |
| `televault` | Launch TUI |

## Configuration

Config stored at `~/.config/televault/config.json`:

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

## Security

- **Encryption**: AES-256-GCM (authenticated encryption)
- **Key Derivation**: Scrypt (memory-hard, GPU-resistant)
- **Session**: Telegram MTProto session stored encrypted
- **Zero Knowledge**: Server never sees unencrypted data
- **MTProto**: Direct Telegram protocol (no bot API limits)

## Prerequisites

- Telegram API credentials (get at [my.telegram.org](https://my.telegram.org))
- Telegram account
- Python 3.11+

## Installation

### From PyPI

```bash
pip install televault
```

### From Source

```bash
git clone https://github.com/YahyaToubali/televault
cd televault
pip install -e .".
```

## Setup

```bash
# Authenticate with Telegram
televault login

# Set up storage channel
televault setup

# Upload your first file
televault push hello.txt
```

## Usage Examples

### Upload a file
```bash
televault push backup.tar.gz --password mysecret
```

### Upload a directory
```bash
televault push ~/Documents/ -r --no-encrypt
```

### Download a file
```bash
televault pull backup.tar.gz --password mysecret
```

### List files
```bash
televault ls --sort=size
```

### Search files
```bash
televault search "*.sql"
```

## CLI Controls

Launch the CLI:

```bash
televault
```

Controls:
- `u` = Upload
- `d` = Download selected
- `l` = List files
- `q` = Quit
- Arrow keys to navigate

## License

MIT

## Author

Yahya Toubali - [@yahyatoubali](https://github.com/YahyaToubali)

---

Built with ❤️ using Python, Telethon, and Textual
