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
# 1) Login with your Telegram account (MTProto, not bot API)
televault login

# 2) Create or attach to a storage channel
televault setup                # creates a new private channel
# or
televault setup -c <channel_id>  # reuse an existing channel

# 3) Upload
TELEVAULT_PASSWORD="strong-password" televault push /path/to/file

# 4) List & download
televault ls
TELEVAULT_PASSWORD="strong-password" televault pull <file_id_or_name>
```

Basic commands:

- `televault login`   – authenticate with Telegram
- `televault setup`   – create/use storage channel
- `televault push`    – upload files / folders (`-r` for recursive)
- `televault pull`    – download by id or name
- `televault ls`      – list files with size/chunks/encryption
- `televault search`  – fuzzy search by name
- `televault status`  – overall vault stats

There is also a TUI entrypoint:

```bash
televault  # interactive terminal UI
```

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
- **Chunked uploads** – Large files split into chunks (up to Telegram’s per-file limit)
- **Resumable transfers** – Can continue interrupted uploads/downloads
- **Folder support** – Upload directories while preserving structure
- **CLI + TUI** – Rich-based progress bars and optional Textual UI

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

## Requirements

- Telegram account + API credentials from [my.telegram.org](https://my.telegram.org)
- Python 3.11 or newer

---

## License

MIT

Author: **Yahya Toubali** · [@yahyatoubali](https://github.com/YahyaToubali)
