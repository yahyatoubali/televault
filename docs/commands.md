# Command Reference

Complete reference for all `tvt` commands in TeleVault v3.5.0 (C++23 Native).

## Core Vault Commands

| Command | Description |
|---|---|
| `tvt push <path>` | Upload a file to the vault |
| `tvt pull <path> [-o <dest>]` | Download a file from vault with atomic replacement |
| `tvt cat <path>` | Stream file content directly to stdout |
| `tvt preview <path>` | Sub-second chunk-0 preview with syntax & MIME detection |
| `tvt ls [--json] [--sort field]` | List all files in the vault |
| `tvt find <query> [--json]` | Search files by name |
| `tvt info <path> [--json]` | Detailed file metadata and chunk topology |
| `tvt stat [--json]` | Vault statistics (total size and file count) |
| `tvt rm <path>` | Delete file and its Telegram chunks |
| `tvt verify <path>` | Verify chunk integrity against channel state |
| `tvt recover` | Reconstruct vault index from channel history |
| `tvt gc [--force] [--clean-partials]` | Garbage collection of orphaned chunks |

### Push / Pull Options

| Flag | Description |
|---|---|
| `-p, --password <pwd>` | Encryption / decryption password (or set `TELEVAULT_PASSWORD`) |
| `--no-encryption` | Upload file unencrypted |
| `-r, --recursive` | Upload directory recursively |
| `--resume` | Resume interrupted transfer |
| `--low-resource` | Low-resource mode (32 MB chunks, reduced parallelism) |
| `-o, --output <dest>` | Output destination for `pull` (`-` for stdout) |

---

## Authentication & Channel Setup

| Command | Description |
|---|---|
| `tvt login` | Interactive login (phone number, code, 2FA, or terminal visual QR code) |
| `tvt logout` | Clear active TDLib session |
| `tvt setup` | Interactive storage channel creator and validator |
| `tvt channel` | Display current storage channel info |
| `tvt whoami` | Display authenticated Telegram account information |

---

## Snapshot Backup Management (`tvt backup`)

TeleVault's backup engine uses dedicated, isolated channel messages that never touch or overwrite your primary vault index.

| Command | Description |
|---|---|
| `tvt backup create <paths...>` | Snapshot files or directories (preserves relative hierarchy) |
| `tvt backup create <paths...> -n <name>` | Assign custom snapshot name |
| `tvt backup create <paths...> --incremental` | Incremental snapshot (skips unchanged files) |
| `tvt backup create <paths...> -p <pwd>` | Custom encryption password |
| `tvt backup list` | List all snapshots with IDs, names, file counts, and sizes |
| `tvt backup restore <id> -o <dest>` | Restore snapshot to destination with path traversal checks |
| `tvt backup restore <id> -p <pwd>` | Decryption password for snapshot restore |
| `tvt backup prune [--keep-daily N]` | Prune snapshots according to GFS retention policy |
| `tvt backup delete <id>` (alias: `rm`) | Permanently delete a snapshot and its message |

---

## Real-Time Directory Watcher (`tvt watch`)

| Command | Description |
|---|---|
| `tvt watch <dir>` | Monitor directory and auto-sync changes to vault |
| `tvt watch <dir> -p <pwd>` | Password for auto-encrypted sync |
| `tvt watch <dir> --exclude <patterns...>` | Patterns to exclude (e.g. `*.tmp`, `.git/*`) |

*Note: Press `Ctrl+C` at any time to shut down the watcher cleanly without orphan locks.*

---

## Global Options

| Option | Description |
|---|---|
| `-h, --help` | Print help message and exit |
| `-V, --version` | Display program version |
| `-v, --verbose` | Verbose log output |
| `--debug` | Enable debug-level diagnostic logging |
