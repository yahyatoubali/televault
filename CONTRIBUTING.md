# Contributing to TeleVault

Thanks for your interest in contributing! This guide will help you get started.

## Quick Start

```bash
git clone https://github.com/YahyaToubali/televault.git
cd televault
python -m venv .venv
source .venv/bin/activate   # Windows: .venv\Scripts\activate
pip install -e ".[dev,fuse,webdav,preview]"
```

## Development Workflow

1. **Fork** the repo and create a branch from `dev`:
   ```bash
   git checkout dev
   git pull origin dev
   git checkout -b feature/my-feature
   ```
2. **Make your changes** with tests
3. **Run checks** before committing:
   ```bash
   pytest tests/ -v          # All 168 tests must pass
   ruff check src/televault/ # Zero lint errors
   ```
4. **Push** and open a PR against `dev`

### Branching

| Branch | Purpose |
|---|---|
| `main` | Stable release code. Only updated via PR from `dev`. |
| `dev` | Integration branch. All PRs target this branch. |
| `feature/*` | Your feature branches. Fork from `dev`, PR back to `dev`. |
| `fix/*` | Bug fix branches. Same flow as features. |

**Never push directly to `main`.** All changes go through PRs.

## Code Style

- **Lint**: `ruff check src/televault/` — must pass with zero errors
- **Line length**: 100 characters max
- **Python**: 3.11+ (type hints required for new code)
- **No comments** unless asked or explaining non-obvious logic
- **Follow existing patterns** in the codebase

## Running Tests

```bash
# Full test suite
pytest tests/ -v

# Single test file
pytest tests/test_chunker.py -v

# With coverage (optional)
pytest tests/ -v --cov=televault
```

Tests use `pytest-asyncio` with `asyncio_mode = "auto"`. All tests are in `tests/` directory.

## Pull Request Checklist

Before opening a PR:

- [ ] Branch is based on `dev` (not `main`)
- [ ] All tests pass: `pytest tests/ -v`
- [ ] Lint passes: `ruff check src/televault/`
- [ ] New features include tests
- [ ] Bug fixes include a test that verifies the fix
- [ ] No unnecessary comments or debug logging

## Reporting Issues

- **Bug**: Use the Bug Report template — include TeleVault version, Python version, OS, and steps to reproduce
- **Feature**: Use the Feature Request template — describe the use case, not just the solution

## Project Structure

```
src/televault/
├── cli.py          # Click CLI — command dispatch, friendly errors
├── core.py         # TeleVault class — upload, download, stream
├── telegram.py     # TelegramVault — MTProto client, index, compression
├── models.py       # FileMetadata, ChunkInfo, VaultIndex, TransferProgress
├── chunker.py      # File splitting, ChunkWriter, BLAKE3
├── crypto.py       # AES-256-GCM, scrypt KDF
├── compress.py     # zstd compression, extension-based skip
├── config.py       # Config dataclass, atomic persistence
├── retry.py        # Exponential backoff, FloodWait handling
├── backup.py       # BackupEngine — snapshot CRUD, prune, verify
├── snapshot.py      # Snapshot, SnapshotFile, SnapshotIndex
├── fuse.py         # TeleVaultFuse — on-demand streaming, LRU cache
├── webdav.py       # WebDAV server (aiohttp)
├── preview.py      # PreviewEngine — terminal previews from headers
├── watcher.py      # FileWatcher — polling, BLAKE2, exclude patterns
├── schedule.py      # Schedule CRUD, systemd timers
├── gc.py            # Orphan message detection and cleanup
├── logging.py       # RotatingFileHandler setup
└── tui.py           # Textual TUI — file browser, detail panel
```

## Key Concepts

- **All data** lives in a private Telegram channel as pinned messages + reply chains
- **Files** are chunked, hashed, compressed, encrypted, then uploaded
- **VaultIndex** maps file IDs to message IDs. Uses `asyncio.Lock` for concurrency safety.
- **Cached index lookups** — `index_msg_id` in memory + config for O(1) fetch
- **Atomic config writes** — temp file + `os.replace` + `fsync`
- **CLI errors** go through `run_async()` — friendly messages, no tracebacks
- **Entry points**: `tvt` and `televault` both resolve to `televault.cli:main`

## Documentation

The docs site is built with MkDocs Material. To preview changes locally:

```bash
pip install -e ".[dev]" && pip install mkdocs-material
mkdocs serve
```

Open http://localhost:8000 to see the live preview. Docs rebuild automatically on file changes.

## License

By contributing, you agree that your code will be licensed under the MIT License.