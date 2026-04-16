# TeleVault

Unlimited encrypted cloud storage via Telegram MTProto. No servers. No limits. No trust required.

```
$ pipx install televault
$ tvt login
$ tvt push secret.tar.gz
```

## Core Specs

| | |
|---|---|
| **Protocol** | Telegram MTProto (Telethon) |
| **Storage** | Private channel, pinned messages + reply chains |
| **Encryption** | AES-256-GCM, scrypt KDF (N=2^17) |
| **Integrity** | BLAKE3 per-chunk + file-level hash |
| **Chunk Size** | 256 MB default (32 MB low-resource) |
| **Parallelism** | 8 uploads / 10 downloads concurrent |
| **Max File** | 2 GB (Telegram limit) |
| **Compression** | zstd level 3 (skips incompressible) |
| **Python** | 3.11+ |

## Quick Start

```bash
# Install
pipx install televault

# Login — will prompt for API credentials from https://my.telegram.org
tvt login

# Setup storage channel (interactive)
tvt setup

# Upload
tvt push photo.jpg
tvt push --recursive /data/documents

# Download
tvt pull photo.jpg
tvt pull photo.jpg -o /tmp/photo.jpg

# List
tvt ls
tvt ls --json | jq '.[].name'

# Stream to stdout
tvt cat config.yaml | grep api_key
cat secret.txt | tvt push - --name secret.txt
```

## Why TeleVault

| | TeleVault | Cloud Storage |
|---|---|---|
| Cost | Free | $5–30/month |
| Limit | Unlimited | 15 GB – 2 TB |
| Encryption | AES-256-GCM client-side | Server-side or none |
| Trust Model | Zero-trust (you hold the key) | Trust the provider |
| Max File | 2 GB | Varies |
| Speed | Parallel chunk transfers | Single connection |

## Next

- **[The Engine](engine.md)** — How data is stored on Telegram: message topology, storage model, encryption pipeline
- **[Security Protocol](security.md)** — AES-256-GCM, scrypt KDF, BLAKE3 integrity, zero-trust model
- **[Hardware Optimization](hardware.md)** — Low-resource mode for constrained systems
- **[Virtualization](virtualization.md)** — FUSE mount and WebDAV server setup
- **[Command Reference](commands.md)** — Complete `tvt` command manual

## Support

TeleVault is free and open source. If you find it useful, consider supporting development:

- [**Ko-fi**](https://ko-fi.com/yahyatoubali) — Buy me a coffee
- [**GitHub Sponsors**](https://github.com/sponsors/yahyatoubali) — Sponsor the project
