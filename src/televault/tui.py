"""Textual TUI for TeleVault - File Browser."""

import asyncio
import contextlib
import logging
import os
import sys

from rich.console import Console
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Container, Horizontal, Vertical
from textual.reactive import reactive
from textual.screen import Screen
from textual.widgets import (
    Button,
    DataTable,
    Footer,
    Header,
    Input,
    Label,
    Static,
)

from .cli import format_size
from .config import Config, get_config_dir as televault_config_dir
from .core import TeleVault

logger = logging.getLogger("televault.tui")

console = Console()

FILE_ICONS = {
    "image": "🖼️",
    "video": "🎬",
    "audio": "🎵",
    "archive": "📦",
    "document": "📄",
    "code": "💻",
    "unknown": "📁",
}


def get_file_icon(filename: str) -> str:
    ext = filename.lower().rsplit(".", 1)[-1] if "." in filename else ""
    image_exts = {"jpg", "jpeg", "png", "gif", "webp", "svg", "bmp", "heic", "heif"}
    video_exts = {"mp4", "mkv", "avi", "mov", "webm", "m4v", "wmv", "flv"}
    audio_exts = {"mp3", "wav", "ogg", "flac", "aac", "m4a", "opus"}
    archive_exts = {"zip", "tar", "gz", "bz2", "xz", "7z", "rar", "zst"}
    code_exts = {"py", "js", "ts", "go", "rs", "c", "cpp", "h", "java", "rb", "php"}
    doc_exts = {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "md"}
    if ext in image_exts:
        return FILE_ICONS["image"]
    if ext in video_exts:
        return FILE_ICONS["video"]
    if ext in audio_exts:
        return FILE_ICONS["audio"]
    if ext in archive_exts:
        return FILE_ICONS["archive"]
    if ext in code_exts:
        return FILE_ICONS["code"]
    if ext in doc_exts:
        return FILE_ICONS["document"]
    return FILE_ICONS["unknown"]


def _cleanup_terminal():
    try:
        sys.stdout.write("\033[?25h")
        sys.stdout.write("\033[0m")
        sys.stdout.write("\033[2J")
        sys.stdout.write("\033[H")
        sys.stdout.flush()
    except Exception:
        pass
    try:
        if sys.stdin.isatty():
            os.system("stty sane 2>/dev/null || true")
    except Exception:
        pass


class VaultApp(App):
    """TeleVault Terminal User Interface."""

    TITLE = "TeleVault"
    SUB_TITLE = "Encrypted Cloud Storage via Telegram"

    CSS = """
    #main-container { layout: horizontal; height: 100%; }
    #sidebar { width: 22; height: 100%; padding: 1 2; border-right: thick $primary; background: $surface; }
    #sidebar .title { text-align: center; color: $text; text-style: bold; margin-bottom: 1; }
    #sidebar .stats-box { padding: 1; margin-bottom: 1; background: $surface-darken-1; border: round $primary; }
    #sidebar .stats-box .title { color: $accent; text-style: bold; }
    .sidebar-button { width: 100%; margin-bottom: 1; }
    #content { width: 1fr; height: 100%; padding: 1 2; }
    #content .title { color: $text; text-style: bold; margin-bottom: 1; }
    #file-table { height: 1fr; }
    #status-bar { dock: bottom; height: 1; background: $primary; color: $text; padding: 0 1; }
    #detail-panel { width: 40; height: 100%; padding: 1 2; border-left: thick $primary; background: $surface; overflow-y: auto; }
    #detail-panel .title { color: $accent; text-style: bold; margin-bottom: 1; }
    .login-container { padding: 2 4; height: auto; }
    .login-container .title { color: $accent; text-style: bold; margin-bottom: 1; }
    .info-text { color: $text-muted; margin-bottom: 1; }
    """

    BINDINGS = [
        Binding("q", "quit", "Quit"),
        Binding("ctrl+c", "quit", "Quit"),
        Binding("r", "refresh", "Refresh"),
        Binding("u", "upload", "Upload"),
        Binding("d", "download", "Download"),
        Binding("s", "search", "Search"),
        Binding("p", "preview", "Preview"),
        Binding("delete", "delete", "Delete"),
    ]

    is_authenticated = reactive(False)
    files = reactive([])

    def __init__(self):
        super().__init__()
        self._vault: TeleVault | None = None
        self._connected = False
        self.config = Config.load_or_create()
        self.selected_file = None
        self._auth_checked = False

    async def _get_vault(self) -> TeleVault:
        if self._vault is None or not self._connected:
            self._vault = TeleVault()
            await self._vault.connect()
            self._connected = True
        return self._vault

    async def _release_vault(self) -> None:
        if self._vault and self._connected:
            with contextlib.suppress(Exception):
                await self._vault.disconnect()
            self._vault = None
            self._connected = False

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Horizontal(id="main-container"):
            with Vertical(id="sidebar"):
                yield Label("📁 TeleVault", classes="title")
                yield Label("")
                with Container(classes="stats-box"):
                    yield Label("📊 Statistics", classes="title")
                    yield Label("Files: 0", id="stat-files")
                    yield Label("Total Size: 0 B", id="stat-size")
                    yield Label("Storage: -", id="stat-storage")
                yield Label("")
                yield Button("📤 Upload", id="btn-upload", classes="sidebar-button")
                yield Button("🔍 Search", id="btn-search", classes="sidebar-button")
                yield Button("🔄 Refresh", id="btn-refresh", classes="sidebar-button")
                yield Button("ℹ️ Status", id="btn-status", classes="sidebar-button")
                yield Button("👤 Whoami", id="btn-whoami", classes="sidebar-button")
            with Vertical(id="content"):
                yield Label("📁 File Browser", classes="title")
                yield Label("Connecting to Telegram...", id="status-label")
        yield Footer()

    async def on_mount(self) -> None:
        self.title = "TeleVault - Encrypted Cloud Storage"
        asyncio.create_task(self._init_auth())

    async def _init_auth(self) -> None:
        try:
            config_path = televault_config_dir() / "telegram.json"
            api_ok = bool(os.environ.get("TELEGRAM_API_ID") and os.environ.get("TELEGRAM_API_HASH"))
            if not api_ok and config_path.exists():
                try:
                    import json

                    with open(config_path) as f:
                        data = json.load(f)
                    if data.get("api_id") and data.get("api_hash"):
                        api_ok = True
                except Exception:
                    pass

            if not api_ok:
                self._update_status("⚠ Not configured. Run: tvt login")
                return

            session_string = None
            if config_path.exists():
                try:
                    import json

                    with open(config_path) as f:
                        data = json.load(f)
                    session_string = data.get("session_string")
                except Exception:
                    pass

            if not session_string:
                self._update_status("⚠ Not logged in. Run: tvt login")
                return

            if self.config.channel_id is None:
                self._update_status("⚠ No channel. Run: tvt setup")
                return

            self._update_status("Connecting...")

            vault = await self._get_vault()
            if not await vault.is_authenticated():
                await self._release_vault()
                self._update_status("⚠ Auth failed. Run: tvt login")
                return

            await vault.telegram.set_channel(self.config.channel_id)
            self.is_authenticated = True
            self._auth_checked = True

            status_label = self.query_one("#status-label", Label)
            status_label.remove()
            search_input = Input(placeholder="Search files...", id="search-input")
            table = DataTable(id="file-table")
            table.add_columns("ID", "Name", "Size", "Chunks", "Encrypted", "Actions")
            table.cursor_type = "row"
            table.zebra_stripes = True
            content = self.query_one("#content", Vertical)
            content.mount(search_input)
            content.mount(table)
            content.mount(
                Static("Press 'r' to refresh, 'u' to upload, 'p' to preview", id="status-bar")
            )

            await self._load_files()
        except Exception as e:
            logger.error(f"Auth init failed: {e}", exc_info=True)
            self._update_status(f"⚠ Error: {str(e)[:60]}")
        finally:
            await self._release_vault()

    def _update_status(self, msg: str) -> None:
        try:
            label = self.query_one("#status-label", Label)
            label.update(msg)
        except Exception:
            try:
                bar = self.query_one("#status-bar", Static)
                bar.update(msg)
            except Exception:
                pass

    async def _load_files(self) -> None:
        if not self._auth_checked:
            return
        try:
            vault = await self._get_vault()
            files = await vault.list_files()
            self.files = files

            table = self.query_one("#file-table", DataTable)
            table.clear()
            for f in files:
                table.add_row(
                    f.id[:8],
                    f.name[:40] + "..." if len(f.name) > 40 else f.name,
                    format_size(f.size),
                    str(f.chunk_count),
                    "🔒" if f.encrypted else "📄",
                    "[Enter] Download | [Del] Delete",
                )

            total_size = sum(f.size for f in files)
            with contextlib.suppress(Exception):
                self.query_one("#stat-files", Label).update(f"Files: {len(files)}")
                self.query_one("#stat-size", Label).update(f"Total: {format_size(total_size)}")

            try:
                status = await vault.get_status()
                stored = status.get("stored_size", 0)
                self.query_one("#stat-storage", Label).update(f"Stored: {format_size(stored)}")
            except Exception:
                pass

            with contextlib.suppress(Exception):
                self.query_one("#status-bar", Static).update(f"✓ {len(files)} files loaded")
        except Exception as e:
            logger.error(f"Error loading files: {e}", exc_info=True)
            self._update_status(f"Error: {str(e)[:60]}")

    def on_unmount(self) -> None:
        _cleanup_terminal()

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        bid = event.button.id
        if bid == "btn-upload":
            await self._do_upload()
        elif bid == "btn-search":
            self.query_one("#search-input", Input).focus()
        elif bid == "btn-refresh":
            asyncio.create_task(self._load_files())
        elif bid == "btn-status":
            await self._show_status()
        elif bid == "btn-whoami":
            await self._show_whoami()

    async def _do_upload(self) -> None:
        self.push_screen(UploadScreen())

    async def _show_status(self) -> None:
        if not self._auth_checked:
            self.notify("Not connected yet", severity="warning")
            return
        try:
            vault = await self._get_vault()
            status = await vault.get_status()
            msg = (
                f"📊 Vault Status\n\n"
                f"Channel: {status.get('channel_id', 'N/A')}\n"
                f"Files: {status.get('file_count', 0)}\n"
                f"Total: {format_size(status.get('total_size', 0))}\n"
                f"Stored: {format_size(status.get('stored_size', 0))}"
            )
            self.notify(msg, title="Status", timeout=8)
        except Exception as e:
            self.notify(f"Error: {str(e)[:60]}", severity="error")

    async def _show_whoami(self) -> None:
        if not self._auth_checked:
            self.notify("Not connected yet", severity="warning")
            return
        try:
            vault = await self._get_vault()
            me = await vault.telegram._client.get_me()
            if me:
                name = f"{me.first_name or ''} {me.last_name or ''}".strip()
                username = f"@{me.username}" if me.username else "N/A"
                self.notify(f"👤 {name}\n{username}\nID: {me.id}", title="Whoami", timeout=8)
        except Exception as e:
            self.notify(f"Error: {str(e)[:60]}", severity="error")

    def action_refresh(self) -> None:
        asyncio.create_task(self._load_files())

    def action_upload(self) -> None:
        self.push_screen(UploadScreen())

    def action_download(self) -> None:
        self.notify("Select a file and press Enter", severity="information")

    def action_preview(self) -> None:
        if not self.files:
            self.notify("No files", severity="warning")
            return
        try:
            table = self.query_one("#file-table", DataTable)
            row_index = table.cursor_row
            if 0 <= row_index < len(self.files):
                self._update_detail(self.files[row_index])
            else:
                self.notify("Select a file", severity="information")
        except Exception:
            self.notify("Select a file", severity="information")

    def action_delete(self) -> None:
        if not self.files:
            self.notify("No files", severity="warning")
            return
        try:
            table = self.query_one("#file-table", DataTable)
            row_index = table.cursor_row
            if 0 <= row_index < len(self.files):
                f = self.files[row_index]

                async def do_delete():
                    try:
                        vault = await self._get_vault()
                        await vault.telegram.set_channel(self.config.channel_id)
                        deleted = await vault.delete(f.id)
                        if deleted:
                            self.notify(f"✓ Deleted {f.name}")
                            await self._load_files()
                        else:
                            self.notify(f"✗ Delete failed", severity="error")
                    except Exception as e:
                        self.notify(f"Error: {str(e)[:60]}", severity="error")
                    finally:
                        await self._release_vault()

                self.push_screen(ConfirmScreen("🗑️ Delete", f"Delete '{f.name}'?", do_delete))
            else:
                self.notify("Select a file", severity="information")
        except Exception:
            self.notify("Select a file", severity="information")

    def action_search(self) -> None:
        try:
            self.query_one("#search-input", Input).focus()
        except Exception:
            pass

    async def on_data_table_row_selected(self, event: DataTable.RowSelected) -> None:
        if not self.files or event.row_index >= len(self.files):
            return
        self.selected_file = self.files[event.row_index]
        self._update_detail(self.selected_file)

    def _update_detail(self, fm) -> None:
        try:
            icon = get_file_icon(fm.name)
            h = fm.hash[:16] + "..." if fm.hash and len(fm.hash) > 16 else (fm.hash or "N/A")
            lines = [
                f"{icon} {fm.name}",
                "",
                f"[bold]ID:[/bold] {fm.id}",
                f"[bold]Size:[/bold] {format_size(fm.size)}",
                f"[bold]Hash:[/bold] {h}",
                f"[bold]Chunks:[/bold] {fm.chunk_count}",
                f"[bold]Encrypted:[/bold] {'Yes' if fm.encrypted else 'No'}",
                f"[bold]Compressed:[/bold] {'Yes' if fm.compressed else 'No'}",
            ]
            if fm.chunks:
                stored = sum(c.size for c in fm.chunks)
                lines.append(f"[bold]Stored:[/bold] {format_size(stored)}")
            try:
                detail_title = self.query_one("#detail-title", Label)
                detail_content = self.query_one("#detail-content", Static)
                detail_title.update(f"📋 {icon} {fm.name[:30]}")
                detail_content.update("\n".join(lines))
            except Exception:
                self.notify(f"{icon} {fm.name} - {format_size(fm.size)}", timeout=4)
        except Exception as e:
            logger.debug(f"Detail update error: {e}")


class UploadScreen(Screen):
    def compose(self) -> ComposeResult:
        with Container(classes="login-container"):
            yield Label("📤 Upload File", classes="title")
            yield Label("")
            yield Label("Enter file path:", classes="info-text")
            yield Input(placeholder="/path/to/file", id="path-input")
            yield Label("")
            yield Label("Password (optional):", classes="info-text")
            yield Input(
                placeholder="Leave empty for no encryption", id="password-input", password=True
            )
            yield Label("")
            yield Static("", id="upload-progress")
            yield Label("")
            with Horizontal():
                yield Button("Upload", id="btn-do-upload", variant="primary")
                yield Button("Cancel", id="btn-cancel")

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-do-upload":
            path = self.query_one("#path-input", Input).value.strip()
            password = self.query_one("#password-input", Input).value or None
            if path:
                await self._upload(path, password)
        elif event.button.id == "btn-cancel":
            self.app.pop_screen()

    async def _upload(self, path: str, password: str | None) -> None:
        progress = self.query_one("#upload-progress", Static)
        vault = None
        try:
            from pathlib import Path

            fp = Path(path)
            if not fp.exists():
                self.app.notify(f"File not found: {path}", severity="error")
                return
            progress.update(f"Uploading {fp.name}...")
            vault = TeleVault(password=password)
            await vault.connect()
            await vault.telegram.set_channel(self.app.config.channel_id)

            def on_progress(p):
                try:
                    progress.update(
                        f"Upload: {p.percent:.1f}% ({p.uploaded_chunks}/{p.total_chunks})"
                    )
                except Exception:
                    pass

            metadata = await vault.upload(path, progress_callback=on_progress)
            progress.update(f"✓ Uploaded: {metadata.name}")
            self.app.notify(f"✓ Uploaded: {metadata.name}")
            await asyncio.sleep(1)
            self.app.pop_screen()
            asyncio.create_task(self.app._load_files())
        except Exception as e:
            progress.update(f"Error: {str(e)[:80]}")
            self.app.notify(f"Upload failed: {str(e)[:60]}", severity="error")
        finally:
            if vault:
                with contextlib.suppress(Exception):
                    await vault.disconnect()


class DownloadScreen(Screen):
    def __init__(self, file_metadata):
        super().__init__()
        self.file_metadata = file_metadata

    def compose(self) -> ComposeResult:
        with Container(classes="login-container"):
            yield Label("📥 Download File", classes="title")
            yield Label("")
            yield Label(f"File: {self.file_metadata.name}", classes="info-text")
            yield Label(f"Size: {format_size(self.file_metadata.size)}")
            yield Label("")
            yield Label("Output path (optional):", classes="info-text")
            yield Input(placeholder="Current directory", id="output-input")
            if self.file_metadata.encrypted:
                yield Label("")
                yield Label("Password:", classes="info-text")
                yield Input(placeholder="Decryption password", id="password-input", password=True)
            yield Label("")
            yield Static("", id="download-progress")
            yield Label("")
            with Horizontal():
                yield Button("Download", id="btn-do-download", variant="primary")
                yield Button("Cancel", id="btn-cancel")

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-do-download":
            output = self.query_one("#output-input", Input).value or None
            pwd = None
            if self.file_metadata.encrypted:
                pwd = self.query_one("#password-input", Input).value or None
            await self._download(output, pwd)
        elif event.button.id == "btn-cancel":
            self.app.pop_screen()

    async def _download(self, output: str | None, password: str | None) -> None:
        progress = self.query_one("#download-progress", Static)
        vault = None
        try:
            progress.update(f"Downloading {self.file_metadata.name}...")
            vault = TeleVault(password=password)
            await vault.connect()
            await vault.telegram.set_channel(self.app.config.channel_id)
            result = await vault.download(self.file_metadata.id, output_path=output)
            progress.update(f"✓ Saved: {result}")
            self.app.notify(f"✓ Downloaded to: {result}")
            await asyncio.sleep(1)
            self.app.pop_screen()
        except Exception as e:
            progress.update(f"Error: {str(e)[:80]}")
            self.app.notify(f"Download failed: {str(e)[:60]}", severity="error")
        finally:
            if vault:
                with contextlib.suppress(Exception):
                    await vault.disconnect()


class ConfirmScreen(Screen):
    def __init__(self, title: str, message: str, on_confirm=None):
        super().__init__()
        self.title_text = title
        self.message = message
        self._on_confirm = on_confirm

    def compose(self) -> ComposeResult:
        with Container(classes="login-container"):
            yield Label(self.title_text, classes="title")
            yield Label("")
            yield Label(self.message, classes="info-text")
            yield Label("")
            with Horizontal():
                yield Button("Confirm", id="btn-confirm", variant="error")
                yield Button("Cancel", id="btn-cancel")

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-confirm":
            self.app.pop_screen()
            if self._on_confirm:
                result = self._on_confirm()
                if asyncio.iscoroutine(result):
                    asyncio.create_task(result)
        elif event.button.id == "btn-cancel":
            self.app.pop_screen()


def run_tui():
    """Run the TUI application with proper terminal cleanup."""
    app = VaultApp()
    try:
        app.run()
    except KeyboardInterrupt:
        _cleanup_terminal()
        print("\nTeleVault TUI exited.")
    except SystemExit:
        _cleanup_terminal()
    except Exception as e:
        _cleanup_terminal()
        print(f"\nTeleVault TUI error: {e}")
        print("If the terminal looks broken, try running: reset")
    else:
        _cleanup_terminal()
