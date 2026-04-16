# Command Reference

Complete reference for all `tvt` commands.

## Core

| Command | Description |
|---|---|
| `tvt push <file>` | Upload file to vault |
| `tvt push - --name <file>` | Upload from stdin |
| `tvt push --recursive <dir>` | Upload directory |
| `tvt pull <file>` | Download file from vault |
| `tvt pull <file> -o -` | Download to stdout |
| `tvt ls` | List all files |
| `tvt ls --json` | List as JSON (pipeable) |
| `tvt ls --sort name` | Sort by name, size, or date |
| `tvt cat <file>` | Stream file to stdout |
| `tvt preview <file>` | Preview without full download |
| `tvt info <file>` | Detailed file information |
| `tvt info <file> --json` | File info as JSON |
| `tvt stat` | Vault statistics |
| `tvt stat --json` | Stats as JSON |
| `tvt find <query>` | Search files by name |
| `tvt find <query> --json` | Search results as JSON |
| `tvt rm <file>` | Delete file |
| `tvt rm <file> --yes` | Delete without confirmation |
| `tvt verify <file>` | Verify file integrity |
| `tvt gc` | Garbage collection (dry-run) |
| `tvt gc --force` | Actually delete orphans |
| `tvt gc --clean-partials` | Remove incomplete uploads |

### Upload Flags

| Flag | Description |
|---|---|
| `--password, -p` | Encryption password |
| `--no-compress` | Disable compression |
| `--no-encrypt` | Disable encryption |
| `--recursive, -r` | Upload directory recursively |
| `--resume` | Resume interrupted upload |
| `--low-resource` | Enable low-resource mode |
| `--if-exists` | Handle duplicates: `version`, `replace`, `skip` |

### Download Flags

| Flag | Description |
|---|---|
| `--output, -o` | Output path (`-o -` for stdout) |
| `--password, -p` | Decryption password |
| `--resume` | Resume interrupted download |
| `--low-resource` | Enable low-resource mode |

## Auth

| Command | Description |
|---|---|
| `tvt login` | Authenticate with Telegram (prompts for API credentials) |
| `tvt login --phone <num>` | Login with specific phone number |
| `tvt logout` | Clear session |
| `tvt setup` | Configure storage channel (interactive) |
| `tvt setup --auto` | Auto-create channel |
| `tvt setup --channel-id <id>` | Use existing channel |
| `tvt whoami` | Show account info |
| `tvt channel` | Show current channel info |

## Backup

| Command | Description |
|---|---|
| `tvt backup create <dir>` | Create backup snapshot |
| `tvt backup create --incremental` | Incremental backup |
| `tvt backup create --dry-run` | Show plan without uploading |
| `tvt backup create --name <name>` | Named snapshot |
| `tvt backup list` | List all snapshots |
| `tvt backup restore <id>` | Restore from snapshot |
| `tvt backup restore --output <dir>` | Restore to specific directory |
| `tvt backup restore --files <f1,f2>` | Restore specific files |
| `tvt backup delete <id>` | Delete snapshot |
| `tvt backup prune` | Prune old snapshots |
| `tvt backup prune --keep-daily 7` | Keep 7 daily snapshots |
| `tvt backup prune --keep-weekly 4` | Keep 4 weekly snapshots |
| `tvt backup prune --keep-monthly 12` | Keep 12 monthly snapshots |
| `tvt backup prune --dry-run` | Show what would be pruned |
| `tvt backup verify <id>` | Verify snapshot integrity |

## Virtual Drive

| Command | Description |
|---|---|
| `tvt mount <path>` | Mount vault as FUSE filesystem |
| `tvt mount --read-only` | Read-only mount |
| `tvt mount --cache-size 500` | 500 MB LRU cache |
| `tvt mount --cache-dir <dir>` | Custom cache directory |
| `tvt mount --allow-other` | Allow other users |
| `tvt mount --foreground` | Run in foreground |
| `tvt mount --background` | Run as daemon |
| `tvt serve` | Start WebDAV server on :8080 |
| `tvt serve --host 0.0.0.0` | Bind to all interfaces |
| `tvt serve --port 9090` | Custom port |
| `tvt serve --read-only` | Read-only WebDAV |

## Automation

| Command | Description |
|---|---|
| `tvt schedule create <dir> --name <n>` | Create backup schedule |
| `tvt schedule create --interval daily` | Daily, weekly, monthly |
| `tvt schedule create --incremental` | Incremental backups |
| `tvt schedule list` | List all schedules |
| `tvt schedule run <name>` | Run schedule immediately |
| `tvt schedule delete <name>` | Delete schedule |
| `tvt schedule install <name>` | Install as systemd timer |
| `tvt schedule uninstall <name>` | Remove systemd timer |
| `tvt schedule show-systemd <name>` | Show systemd unit files |
| `tvt watch --path <dir>` | Watch directory for changes |
| `tvt watch --interval 10` | Poll interval in seconds |
| `tvt watch --exclude "*.tmp"` | Exclude patterns |

## TUI

| Command | Description |
|---|---|
| `tvt tui` | Launch interactive terminal UI (beta) |

## Global Flags

| Flag | Description |
|---|---|
| `-v, --verbose` | INFO-level logging |
| `--debug` | DEBUG-level logging |
| `--version` | Show version |
| `--help` | Show help |

## Pipeable Commands

```bash
# Upload from stdin
cat secret.txt | tvt push - --name secret.txt

# Stream to stdout
tvt cat config.yaml | grep api_key

# JSON output with jq
tvt ls --json | jq '.[].name'
tvt stat --json | jq '.total_files'
tvt info photo.jpg --json | jq '.size'
tvt find report --json | jq 'length'
```

## Error Handling

All CLI errors produce friendly messages — no Python tracebacks:

| Error | Message |
|---|---|
| No internet | "Check your internet connection" |
| Not connected | "Run `tvt login` first" |
| FloodWait | "Wait a few minutes and try again" |
| Session expired | "Session expired, run `tvt login`" |
| Invalid API ID | "Check your API credentials" |
| Ctrl+C | Clean "Interrupted" exit |
