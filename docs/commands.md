# Command Reference

Complete reference for all `tvt` commands in TeleVault v4.0.3 (C++23 Native).

## Core Vault Commands

| Command | Description |
|---|---|
| `tvt push <path>` | Upload a file with FastCDC content-defined chunking and deduplication |
| `tvt pull [query] [-o <dest>]` | Smart download: interactive menu if omitted, fuzzy matching, auto destination |
| `tvt stream <path> [--port 8080]` | Stream media directly over HTTP Range (206 Partial Content) to VLC/browser |
| `tvt cat <path>` | Stream file content directly to stdout |
| `tvt preview <path>` | Sub-second chunk-0 preview with syntax & MIME detection |
| `tvt ls [-w] [--json] [--sort field]` | List files with dynamic terminal width or wide mode (`-w`) |
| `tvt find <query> [-e ext] [--min-size N]` | Search files with extension, size filters, and substring highlighting |
| `tvt info <path> [--json]` | Detailed file metadata and chunk topology |
| `tvt stat [--json]` | Vault statistics (total size and file count) |
| `tvt rm <path>` | Delete file and its Telegram chunks |
| `tvt verify <path>` | Verify chunk integrity against channel state |
| `tvt recover` | Reconstruct vault index from channel history |
| `tvt gc [--force] [--clean-partials]` | Garbage collection of orphaned chunks |
| `tvt completion <shell>` | Generate shell autocompletion script (`bash`, `zsh`, `fish`) |

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

## Interactive Terminal UI (`tvt tui`)

TeleVault includes a high-performance native Terminal User Interface built with [FTXUI](https://github.com/ArthurSonzogni/FTXUI).

```bash
tvt tui
```

### Keybindings & Navigation

| Key | Action |
|---|---|
| `↑` / `k` | Move selection up in file list |
| `↓` / `j` | Move selection down in file list |
| `PgUp` / `PgDn` | Scroll file list by 10 items |
| `Home` / `End` | Jump to beginning / end of file list |
| `/` | Focus real-time search & filter input |
| `u` | Open interactive Upload modal (with encryption/compression toggles) |
| `d` | Open interactive Download modal with destination & live gauge |
| `p` | Sub-second chunk 0 preview (syntax text & hex dump viewer) |
| `x` / `Del` | Delete selected file with confirmation dialog |
| `r` / `F5` | Refresh vault index from Telegram |
| `?` / `F1` | Open keybinding help modal |
| `Esc` | Dismiss modal or clear search input |
| `q` | Exit TUI |

---

## Authentication & Channel Setup


| Command | Description |
|---|---|
| `tvt login` | Interactive login (phone number, code, 2FA, or terminal visual QR code) |
| `tvt login --qr` | QR-code login: scan in-terminal code, no SMS round-trip |
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
