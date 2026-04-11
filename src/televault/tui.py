"""Textual TUI for TeleVault."""

import asyncio
import contextlib
import logging
import os

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
from .config import Config
from .config import get_config_dir as televault_config_dir
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
    ext = filename.lower().split(".")[-1] if "." in filename else ""

    image_exts = {"jpg", "jpeg", "png", "gif", "webp", "svg", "bmp", "heic", "heif"}
    video_exts = {"mp4", "mkv", "avi", "mov", "webm", "m4v", "wmv", "flv"}
    audio_exts = {"mp3", "wav", "ogg", "flac", "aac", "m4a", "opus"}
    archive_exts = {"zip", "tar", "gz", "bz2", "xz", "7z", "rar", "zst"}
    code_exts = {"py", "js", "ts", "go", "rs", "c", "cpp", "h", "java", "rb", "php"}
    doc_exts = {"pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "md"}

    if ext in image_exts:
        return FILE_ICONS["image"]
    elif ext in video_exts:
        return FILE_ICONS["video"]
    elif ext in audio_exts:
        return FILE_ICONS["audio"]
    elif ext in archive_exts:
        return FILE_ICONS["archive"]
    elif ext in code_exts:
        return FILE_ICONS["code"]
    elif ext in doc_exts:
        return FILE_ICONS["document"]
    else:
        return FILE_ICONS["unknown"]


class VaultApp(App):
    """TeleVault Terminal User Interface."""

    TITLE = "TeleVault"
    SUB_TITLE = "Encrypted Cloud Storage via Telegram"

    CSS = """
    #main-container {
        layout: horizontal;
        height: 100%;
    }

    #sidebar {
        width: 22;
        height: 100%;
        padding: 1 2;
        border-right: thick $primary;
        background: $surface;
    }

    #sidebar .title {
        text-align: center;
        color: $text;
        text-style: bold;
        margin-bottom: 1;
    }

    #sidebar .stats-box {
        padding: 1;
        margin-bottom: 1;
        background: $surface-darken-1;
        border: round $primary;
    }

    #sidebar .stats-box .title {
        color: $accent;
        text-style: bold;
    }

    .sidebar-button {
        width: 100%;
        margin-bottom: 1;
    }

    #content {
        width: 1fr;
        height: 100%;
        padding: 1 2;
    }

    #content .title {
        color: $text;
        text-style: bold;
        margin-bottom: 1;
    }

    #file-table {
        height: 1fr;
    }

    #status-bar {
        dock: bottom;
        height: 1;
        background: $primary;
        color: $text;
        padding: 0 1;
    }

    #detail-panel {
        width: 40;
        height: 100%;
        padding: 1 2;
        border-left: thick $primary;
        background: $surface;
        overflow-y: auto;
    }

    #detail-panel .title {
        color: $accent;
        text-style: bold;
        margin-bottom: 1;
    }

    #detail-panel .field-label {
        color: $text-muted;
    }

    #detail-panel .field-value {
        color: $text;
        margin-bottom: 1;
    }

    #detail-panel .preview-box {
        background: $surface-darken-1;
        border: round $primary;
        padding: 1;
        margin-top: 1;
        max-height: 16;
        overflow-y: auto;
    }

    #setup-panel {
        padding: 2 4;
        height: 100%;
    }

    #setup-panel .title {
        color: $accent;
        text-style: bold;
        margin-bottom: 1;
    }

    #setup-panel .step {
        color: $text;
        margin-bottom: 1;
    }

    #setup-panel .cmd {
        color: $accent;
        text-style: bold;
    }

    #setup-panel .hint {
        color: $text-muted;
        margin-bottom: 1;
    }
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
    has_channel = reactive(False)
    files = reactive([])

    def __init__(self):
        super().__init__()
        self._vault: TeleVault | None = None
        self._connected = False
        self.config = Config.load_or_create()
        self.selected_file = None

    async def _get_vault(self) -> TeleVault:
        if self._vault is None or not self._connected:
            self._vault = TeleVault()
            await self._vault.connect()
            self._connected = True
        return self._vault

    async def _release_vault(self) -> None:
        if self._vault and self._connected:
            await self._vault.disconnect()
            self._vault = None
            self._connected = False

    status_message = reactive("Ready")

    def on_unmount(self) -> None:
        if self._vault and self._connected:
            try:
                loop = asyncio.get_event_loop()
                if loop.is_running():
                    loop.create_task(self._vault.disconnect())
                else:
                    loop.run_until_complete(self._vault.disconnect())
            except Exception:
                pass
            self._vault = None
            self._connected = False

    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        yield from self._compose_main_screen()
        yield Footer()

    def _compose_main_screen(self) -> ComposeResult:
        with Container(id="main-container"):
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
                yield Input(placeholder="Search files...", id="search-input")

                table = DataTable(id="file-table")
                table.add_columns("ID", "Name", "Size", "Chunks", "Encrypted", "Actions")
                table.cursor_type = "row"
                table.zebra_stripes = True
                yield table

                yield Static(
                    "Ready - Press 'q' to quit, 'p' to preview, 'u' to upload",
                    id="status-bar",
                )

            with Vertical(id="detail-panel"):
                yield Label("📋 File Details", id="detail-title", classes="title")
                yield Static("Select a file to see details", id="detail-content")

    async def on_mount(self) -> None:
        self.title = "TeleVault - Encrypted Cloud Storage"

        config_path = televault_config_dir() / "telegram.json"
        api_configured = os.environ.get("TELEGRAM_API_ID") and os.environ.get("TELEGRAM_API_HASH")
        if not api_configured and config_path.exists():
            try:
                import json

                with open(config_path) as f:
                    data = json.load(f)
                if data.get("api_id") and data.get("api_hash"):
                    api_configured = True
                if data.get("session_string"):
                    pass
            except Exception:
                pass

        if not api_configured:
            self.is_authenticated = False
            self.has_channel = False
            self.status_message = "Not configured"
            self._show_setup_hint("api")
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
            self.is_authenticated = False
            self.status_message = "Not logged in"
            self._show_setup_hint("login")
            return

        has_channel = self.config.channel_id is not None
        if not has_channel:
            self.has_channel = False
            self.status_message = "No channel"
            self._show_setup_hint("channel")
            return

        await self._check_auth()

    def _show_setup_hint(self, step: str) -> None:
        try:
            status_bar = self.query_one("#status-bar", Static)
            if step == "api":
                status_bar.update("⚠ Not configured. Run: tvt login  |  Press 'q' to quit")
            elif step == "login":
                status_bar.update("⚠ Not logged in. Run: tvt login  |  Press 'q' to quit")
            elif step == "channel":
                status_bar.update("⚠ No channel. Run: tvt setup  |  Press 'q' to quit")
        except Exception:
            pass

    async def _check_auth(self) -> None:
        try:
            vault = await self._get_vault()

            if await vault.is_authenticated():
                self.is_authenticated = True
                self.has_channel = True
                await self._load_files()
            else:
                await self._release_vault()
                self.is_authenticated = False
                self.status_message = "Not logged in - Run: tvt login"
        except Exception as e:
            logger.warning(f"Auth check failed: {e}")
            await self._release_vault()
            self.is_authenticated = False
            self.status_message = "Connection error - Run: tvt login"
            self.notify(f"Could not connect: {str(e)[:80]}", severity="error")

    async def _load_files(self) -> None:
        if not self.is_authenticated:
            return

        try:
            self.status_message = "Loading files..."
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
            self.query_one("#stat-files", Label).update(f"Files: {len(files)}")
            self.query_one("#stat-size", Label).update(f"Total Size: {format_size(total_size)}")

            try:
                status = await vault.get_status()
                stored = status.get("stored_size", 0)
                self.query_one("#stat-storage", Label).update(f"Stored: {format_size(stored)}")
            except Exception:
                pass

            self.status_message = f"Loaded {len(files)} files"

        except Exception as e:
            self.status_message = f"Error loading files: {str(e)}"

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        button_id = event.button.id

        if button_id == "btn-upload":
            await self._do_upload()
        elif button_id == "btn-search":
            await self._do_search()
        elif button_id == "btn-refresh":
            await self._load_files()
        elif button_id == "btn-status":
            await self._show_status()
        elif button_id == "btn-whoami":
            await self._show_whoami()

    async def _do_upload(self) -> None:
        if not self.is_authenticated:
            self.notify("Not logged in. Run: tvt login", severity="error")
            return
        self.push_screen(UploadScreen())

    async def _do_search(self) -> None:
        if not self.is_authenticated:
            self.notify("Not logged in. Run: tvt login", severity="error")
            return
        self.query_one("#search-input", Input).focus()

    async def _show_status(self) -> None:
        if not self.is_authenticated:
            self.notify("Not logged in. Run: tvt login", severity="error")
            return

        try:
            vault = await self._get_vault()
            status = await vault.get_status()

            channel_id = status.get("channel_id", "N/A")
            file_count = status.get("file_count", 0)
            total_size = status.get("total_size", 0)
            stored_size = status.get("stored_size", 0)
            ratio = status.get("compression_ratio", 0)
            ratio_str = f"{ratio:.1%}" if isinstance(ratio, (int, float)) else "N/A"

            message = f"""
📊 Vault Status

Channel ID: {channel_id}
Files: {file_count}
Total Size: {format_size(total_size)}
Stored Size: {format_size(stored_size)}
Compression: {ratio_str}
            """.strip()

            self.notify(message, title="Status", timeout=10)
        except Exception as e:
            self.notify(f"Error: {str(e)}", severity="error")

    async def _show_whoami(self) -> None:
        if not self.is_authenticated:
            self.notify("Not logged in. Run: tvt login", severity="error")
            return

        try:
            vault = await self._get_vault()
            if not await vault.is_authenticated():
                self.notify("Not logged in. Run: tvt login", severity="error")
                return
            me = await vault.telegram._client.get_me()

            if me:
                name = f"{me.first_name or ''} {me.last_name or ''}".strip()
                username = f"@{me.username}" if me.username else "No username"

                message = f"""
👤 Account Info

Name: {name}
Username: {username}
ID: {me.id}
                """.strip()

                self.notify(message, title="Whoami", timeout=10)
        except Exception as e:
            self.notify(f"Error: {str(e)}", severity="error")

    def action_refresh(self) -> None:
        asyncio.create_task(self._load_files())

    def action_upload(self) -> None:
        asyncio.create_task(self._do_upload())

    def action_download(self) -> None:
        self.notify("Select a file and press Enter to download", severity="information")

    def action_preview(self) -> None:
        if not self.files:
            self.notify("No files to preview", severity="warning")
            return

        try:
            table = self.query_one("#file-table", DataTable)
            row_index = table.cursor_row
            if 0 <= row_index < len(self.files):
                file_meta = self.files[row_index]
                self._update_detail_panel(file_meta)
            else:
                self.notify("Select a file to preview", severity="information")
        except Exception:
            self.notify("Select a file to preview", severity="information")

    def _update_detail_panel(self, file_meta) -> None:
        try:
            detail_title = self.query_one("#detail-title", Label)
            detail_content = self.query_one("#detail-content", Static)

            icon = get_file_icon(file_meta.name)
            from datetime import datetime

            created_str = (
                datetime.fromtimestamp(file_meta.created_at).strftime("%Y-%m-%d %H:%M")
                if file_meta.created_at
                else "Unknown"
            )
            hash_str = (
                file_meta.hash[:16] + "..."
                if file_meta.hash and len(file_meta.hash) > 16
                else (file_meta.hash or "N/A")
            )

            lines = []
            lines.append(f"{icon} {file_meta.name}")
            lines.append("")
            lines.append(f"[bold]ID:[/bold] {file_meta.id}")
            lines.append(f"[bold]Size:[/bold] {format_size(file_meta.size)}")
            lines.append(f"[bold]Hash:[/bold] {hash_str}")
            lines.append(f"[bold]Chunks:[/bold] {file_meta.chunk_count}")
            lines.append(f"[bold]Encrypted:[/bold] {'Yes' if file_meta.encrypted else 'No'}")
            lines.append(f"[bold]Compressed:[/bold] {'Yes' if file_meta.compressed else 'No'}")
            if file_meta.compressed and file_meta.compression_ratio:
                lines.append(f"[bold]Comp. ratio:[/bold] {file_meta.compression_ratio:.1%}")
            lines.append(f"[bold]Created:[/bold] {created_str}")
            if file_meta.mime_type:
                lines.append(f"[bold]MIME:[/bold] {file_meta.mime_type}")
            if file_meta.chunks:
                stored = sum(c.size for c in file_meta.chunks)
                lines.append(f"[bold]Stored:[/bold] {format_size(stored)}")

            detail_title.update(f"📋 {icon} {file_meta.name[:30]}")
            detail_content.update("\n".join(lines))

        except Exception as e:
            logger.debug(f"Error updating detail panel: {e}")

    def action_delete(self) -> None:
        if not self.files:
            self.notify("No files to delete", severity="warning")
            return

        try:
            table = self.query_one("#file-table", DataTable)
            row_index = table.cursor_row
            if 0 <= row_index < len(self.files):
                file_to_delete = self.files[row_index]

                async def do_delete():
                    try:
                        vault = await self._get_vault()
                        deleted = await vault.delete(file_to_delete.id)

                        if deleted:
                            self.notify(f"✓ Deleted: {file_to_delete.name}")
                            await self._load_files()
                        else:
                            self.notify(
                                f"✗ Failed to delete: {file_to_delete.name}", severity="error"
                            )
                    except Exception as e:
                        self.notify(f"Error: {str(e)}", severity="error")

                self.push_screen(
                    ConfirmScreen(
                        "🗑️ Confirm Delete",
                        f"Delete '{file_to_delete.name}'? Cannot be undone.",
                        do_delete,
                    )
                )
            else:
                self.notify("Select a file to delete", severity="information")
        except Exception:
            self.notify("Select a file to delete", severity="information")

    def action_search(self) -> None:
        asyncio.create_task(self._do_search())

    async def on_data_table_row_selected(self, event: DataTable.RowSelected) -> None:
        if not self.files:
            return

        row_index = event.row_index
        if 0 <= row_index < len(self.files):
            self.selected_file = self.files[row_index]
            self._update_detail_panel(self.selected_file)

    async def _download_file(self, file_metadata) -> None:
        self.push_screen(DownloadScreen(file_metadata))

    def watch_status_message(self, message: str) -> None:
        try:
            status_bar = self.query_one("#status-bar", Static)
            status_bar.update(message)
        except Exception:
            pass


class UploadScreen(Screen):
    """Screen for uploading files."""

    def compose(self) -> ComposeResult:
        with Container(classes="login-container"):
            yield Label("📤 Upload File", classes="title")
            yield Label("")
            yield Label("Enter file path:", classes="info-text")
            yield Input(placeholder="/path/to/file", id="path-input")
            yield Label("")
            yield Label("Password (optional):", classes="info-text")
            yield Input(
                placeholder="Leave empty to use env var", id="password-input", password=True
            )
            yield Label("")
            yield Static("", id="upload-progress")
            yield Label("")
            with Horizontal():
                yield Button("Upload", id="btn-do-upload", variant="primary")
                yield Button("Cancel", id="btn-cancel")

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-do-upload":
            path = self.query_one("#path-input", Input).value
            password = self.query_one("#password-input", Input).value
            if path:
                await self._upload_file(path, password or None)
        elif event.button.id == "btn-cancel":
            self.app.pop_screen()

    async def _upload_file(self, path: str, password: str | None) -> None:
        progress_label = self.query_one("#upload-progress", Static)
        vault = None
        try:
            from pathlib import Path

            file_path = Path(path)
            if not file_path.exists():
                self.app.notify(f"File not found: {path}", severity="error")
                return

            progress_label.update(f"Uploading {file_path.name}...")

            vault = TeleVault(password=password)
            await vault.connect()

            def on_progress(p):
                try:
                    asyncio.get_running_loop()
                    progress_label.update(
                        f"Upload: {p.percent:.1f}% ({p.uploaded_chunks}/{p.total_chunks})"
                    )
                except RuntimeError:
                    pass

            metadata = await vault.upload(path, progress_callback=on_progress)

            progress_label.update(f"✓ Uploaded: {metadata.name}")
            self.app.notify(f"✓ Uploaded: {metadata.name}")

            await asyncio.sleep(1)
            self.app.pop_screen()

            await self.app._load_files()

        except Exception as e:
            progress_label.update(f"Error: {str(e)}")
            self.app.notify(f"Upload failed: {str(e)}", severity="error")
        finally:
            if vault:
                with contextlib.suppress(Exception):
                    await vault.disconnect()


class DownloadScreen(Screen):
    """Screen for downloading files."""

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
            yield Label("")
            if self.file_metadata.encrypted:
                yield Label("Password:", classes="info-text")
                yield Input(
                    placeholder="Enter decryption password", id="password-input", password=True
                )
                yield Label("")
            yield Static("", id="download-progress")
            yield Label("")
            with Horizontal():
                yield Button("Download", id="btn-do-download", variant="primary")
                yield Button("Cancel", id="btn-cancel")

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-do-download":
            output = self.query_one("#output-input", Input).value
            password_input = (
                self.query_one("#password-input", Input) if self.file_metadata.encrypted else None
            )
            password = password_input.value if password_input else None
            await self._download_file(output or None, password or None)
        elif event.button.id == "btn-cancel":
            self.app.pop_screen()

    async def _download_file(self, output: str | None, password: str | None) -> None:
        progress_label = self.query_one("#download-progress", Static)
        vault = None
        try:
            progress_label.update(f"Downloading {self.file_metadata.name}...")

            vault = TeleVault(password=password)
            await vault.connect()

            def on_progress(p):
                try:
                    asyncio.get_running_loop()
                    progress_label.update(
                        f"Download: {p.percent:.1f}% ({p.downloaded_chunks}/{p.total_chunks})"
                    )
                except RuntimeError:
                    pass

            output_path = await vault.download(
                self.file_metadata.id, output_path=output, progress_callback=on_progress
            )

            progress_label.update(f"✓ Downloaded to: {output_path}")
            self.app.notify(f"✓ Downloaded to: {output_path}")

            await asyncio.sleep(1)
            self.app.pop_screen()

        except Exception as e:
            progress_label.update(f"Error: {str(e)}")
            self.app.notify(f"Download failed: {str(e)}", severity="error")
        finally:
            if vault:
                with contextlib.suppress(Exception):
                    await vault.disconnect()


class ConfirmScreen(Screen):
    """Screen for confirming destructive actions."""

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
    import os
    import sys

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
    finally:
        try:
            if (
                hasattr(app, "_vault")
                and hasattr(app, "_connected")
                and app._vault
                and app._connected
            ):
                import asyncio

                try:
                    loop = asyncio.get_event_loop()
                    loop.run_until_complete(app._vault.disconnect())
                except Exception:
                    pass
                app._vault = None
                app._connected = False
        except Exception:
            pass


if __name__ == "__main__":
    run_tui()
