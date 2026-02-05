# TeleVault

Unlimited cloud storage using your **own** Telegram account. No local database — everything lives in a private Telegram channel, encrypted on your machine before upload.

---

## Install

```bash
pip install televault
```

Python 3.11+ is recommended.

---

## Quick Start

```bash
# 1) Set up your Telegram API credentials (one-time setup)
export TELEGRAM_API_ID=your_api_id
export TELEGRAM_API_HASH=your_api_hash

# 2) Login with your Telegram account (MTProto, not bot API)
televault login

# 3) Set up storage channel (interactive)
televault setup

# 4) Upload
TELEVAULT_PASSWORD="strong-password" televault push /path/to/file

# 5) List & download
televault ls
TELEVAULT_PASSWORD="strong-password" televault pull <file_id_or_name>
```

Basic commands:

- `televault login`   – authenticate with Telegram
- `televault setup`   – set up storage channel (interactive or CLI flags)
- `televault push`    – upload files / folders (`-r` for recursive)
- `televault pull`    – download by id or name
- `televault ls`      – list files with size/chunks/encryption
- `televault search`  – fuzzy search by name
- `televault status`  – overall vault stats
- `televault whoami`  – show current Telegram account
- `televault logout`  – clear session

All commands check authentication first and will prompt you to run `televault login` if needed.

---

## Interactive TUI

TeleVault includes a rich Terminal User Interface (TUI) for visual file management:

```bash
# Launch the TUI
televault tui
# or
televault-tui
```

### TUI Features:

- **📁 File Browser** – Browse all files with details (size, chunks, encryption status)
- **🔍 Search** – Real-time search through your files
- **📤 Upload** – Interactive file upload with password protection
- **📥 Download** – One-click file download
- **📊 Statistics** – View vault stats (total files, storage used)
- **⌨️ Keyboard Shortcuts**:
  - `q` - Quit
  - `r` - Refresh file list
  - `u` - Upload file
  - `d` - Download selected file
  - `s` - Search files
  - `l` - Login
  - `Delete` - Delete selected file
  - `Enter` - Download selected file

The TUI provides a more visual and interactive way to manage your vault compared to the CLI commands.

---

## Storage Channel Setup

The `televault setup` command provides three ways to configure your storage:

### Interactive Mode (Recommended)
```bash
televault setup
```

You'll be prompted to choose:
```
TeleVault Storage Channel Setup

How would you like to set up your storage?
  1. Create a new private channel (recommended)
  2. Use an existing channel by ID

Enter your choice (1 or 2):
```

### Non-Interactive Options

**Auto-create a new channel:**
```bash
televault setup --auto-create
```

**Use an existing channel:**
```bash
televault setup --channel-id -1001234567890
```

> **Note:** Channel IDs should start with `-100` (e.g., `-1001234567890`). Make sure the bot is a member of the channel if using an existing one.

---

## Project Vision

TeleVault is not a SaaS. The goal is to give hackers and power users a **simple, encrypted off‑site backup tool** built on top of infrastructure they already use every day.

- **Turn Telegram into your personal encrypted blob store** instead of spinning up S3 buckets, servers, or dashboards.
- **Stay client‑side by design** – TeleVault handles chunking, indexing, and crypto; Telegram just stores opaque data.
- **Be as easy to adopt as `pip install` + `televault login`**, with no extra services to maintain.
- **Stay portable** – future tooling should make it easy to export / migrate data so you’re never locked into Telegram.

This should feel closer to `restic`/`borg` than a cloud app: a sharp, scriptable tool that respects your threat model.

---

## Features

- **MTProto direct** – Uses Telethon + MTProto (no bot API limits)
- **Encrypted-by-default** – Client-side AES‑256‑GCM, password-derived keys
- **Zero local DB** – Metadata index is stored on Telegram itself
- **Chunked uploads** – Large files split into chunks (up to Telegram's per-file limit)
- **Resumable transfers** – Can continue interrupted uploads/downloads
- **Folder support** – Upload directories while preserving structure
- **Rich CLI** – Progress bars, colored output, and helpful error messages
- **Interactive TUI** – Full terminal UI with file browser, search, and management
- **Interactive setup** – Choose between creating new channel or using existing one

---

## Security Model

TeleVault is designed so that Telegram sees only encrypted blobs and JSON metadata; your password never leaves your machine.

- **Encryption**
  - Files are optionally compressed and then encrypted with **AES‑256‑GCM**.
  - Keys are derived from your password using **scrypt** (memory‑hard KDF).
  - Encryption happens **before** data is sent to Telegram.

- **Indexing & Metadata**
  - Each file has a small JSON metadata message (size, name, chunk ids, hash).
  - A pinned "index" message in your channel maps file IDs → Telegram message IDs.
  - No external database or server is required.

- **Sessions & Accounts**
  - TeleVault authenticates using a standard Telethon session file.
  - That session lives in your config directory (see below) and is not uploaded.

- **Threat Model (summary)**
  - If someone gets access to your Telegram account **and** your password, they can read your data.
  - If they only get Telegram’s servers or just the channel history, they only see encrypted chunks + metadata.

> Important: **Don’t lose your password.** There is no recovery if you forget it and have encryption enabled.

---

## Configuration

Configuration is stored under:

```text
~/.config/televault/config.json
```

Example:

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

You can override encryption on a per-command basis:

```bash
# disable encryption for this upload only
televault push movie.mkv --no-encrypt

# disable compression
televault push backup.tar --no-compress
```

The default password can also be set via environment variable:

```bash
export TELEVAULT_PASSWORD="strong-password"
televault push secrets.zip
```

---

## From Source (dev)

```bash
git clone https://github.com/YahyaToubali/televault.git
cd televault

# create an isolated env (recommended)
python -m venv .venv
source .venv/bin/activate

pip install -e .[dev]
```

Run tests:

```bash
pytest
```

---

## Roadmap (early ideas)

This is intentionally small for now; priorities will change as you use it.

- **Snapshots & versioning**
  - `televault snapshot ~/Projects` with named snapshots and retention rules.
  - Simple policies like "keep daily snapshots for 7 days, weekly for 4 weeks".
- **Smarter TUI**
  - Better file browser, filters (by size/date/encrypted), and batch actions.
- **Scheduling helpers**
  - Tiny helper commands or docs for cron/systemd timers ("backup this folder nightly").
- **Export / migrate tooling**
  - One‑shot export from Telegram → local disk or other storage (S3, filesystem, etc.).
- **Multi‑vault support**
  - Multiple channels as separate vaults (e.g. `personal`, `work`, `archives`).

If you’re reading this on GitHub and want to use TeleVault seriously, open an issue with your use case so priorities can be adjusted.

---

## Requirements

- Telegram account + API credentials from [my.telegram.org](https://my.telegram.org)
  - Create an app to get your `api_id` and `api_hash`
  - Set them as environment variables:
    ```bash
    export TELEGRAM_API_ID=your_api_id
    export TELEGRAM_API_HASH=your_api_hash
    ```
- Python 3.11 or newer

> **Tip:** Add the export lines to your `~/.bashrc`, `~/.zshrc`, or `~/.profile` to make them persistent.

---

## License

MIT

Author: **Yahya Toubali** · [@yahyatoubali](https://github.com/YahyaToubali)
