"""Textual TUI for TeleVault."""

import asyncio
from pathlib import Path

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
from .core import TeleVault

console = Console()


class VaultApp(App):
    """Main TeleVault TUI Application."""

    CSS = """
    Screen {
        align: center middle;
    }

    #main-container {
        width: 100%;
        height: 100%;
    }

    #sidebar {
        width: 25;
        height: 100%;
        dock: left;
        background: $surface-darken-1;
        padding: 1;
    }

    #content {
        width: 100%;
        height: 100%;
        padding: 1;
    }

    .sidebar-button {
        width: 100%;
        margin: 1 0;
    }

    .stats-box {
        height: auto;
        padding: 1;
        background: $surface-darken-2;
        border: solid $primary;
        margin: 1 0;
    }

    DataTable {
        height: 100%;
    }

    #status-bar {
        dock: bottom;
        height: 3;
        background: $surface-darken-1;
        color: $text;
        padding: 0 2;
        content-align: center middle;
    }

    #search-input {
        width: 100%;
        margin: 1 0;
    }

    .title {
        text-align: center;
        text-style: bold;
        color: $primary;
    }

    ProgressBar {
        width: 100%;
        margin: 1 0;
    }

    #login-screen {
        align: center middle;
    }

    .login-container {
        width: 60;
        height: auto;
        border: solid $primary;
        padding: 2;
        background: $surface;
    }

    .info-text {
        color: $text-muted;
        text-align: center;
    }
    """

    BINDINGS = [
        Binding("q", "quit", "Quit", show=True),
        Binding("r", "refresh", "Refresh", show=True),
        Binding("u", "upload", "Upload", show=True),
        Binding("d", "download", "Download", show=True),
        Binding("delete", "delete", "Delete", show=True),
        Binding("s", "search", "Search", show=True),
        Binding("l", "login", "Login", show=True),
    ]

    vault = reactive(None)
    files = reactive([])
    status_message = reactive("Ready")
    is_authenticated = reactive(False)

    def __init__(self):
        super().__init__()
        self.vault_instance = None
        self.config = Config.load_or_create()
        self.selected_file = None

    def compose(self) -> ComposeResult:
        """Compose the main UI."""
        yield Header(show_clock=True)

        if not self.is_authenticated:
            yield from self._compose_login_screen()
        else:
            yield from self._compose_main_screen()

        yield Footer()

    def _compose_login_screen(self) -> ComposeResult:
        """Compose the login screen."""
        with Container(id="login-screen"), Container(classes="login-container"):
            yield Label("TeleVault", classes="title")
            yield Label("")
            yield Label("Welcome to TeleVault", classes="info-text")
            yield Label("Your encrypted Telegram cloud storage", classes="info-text")
            yield Label("")

            yield Label("Status: Not Authenticated", id="auth-status")
            yield Label("")

            with Horizontal():
                yield Button("🔐 Login", id="btn-login", variant="primary")
                yield Button("❌ Exit", id="btn-exit", variant="error")

            yield Label("")
            yield Label("Press Ctrl+C to exit anytime", classes="info-text")

    def _compose_main_screen(self) -> ComposeResult:
        """Compose the main application screen."""
        with Container(id="main-container"):
            # Sidebar
            with Vertical(id="sidebar"):
                yield Label("📁 TeleVault", classes="title")
                yield Label("")

                # Stats
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
                yield Button("🔓 Logout", id="btn-logout", classes="sidebar-button")

            # Main content
            with Vertical(id="content"):
                yield Label("📁 File Browser", classes="title")
                yield Input(placeholder="Search files...", id="search-input")

                # File table
                table = DataTable(id="file-table")
                table.add_columns("ID", "Name", "Size", "Chunks", "Encrypted", "Actions")
                table.cursor_type = "row"
                table.zebra_stripes = True
                yield table

                # Status bar
                yield Static(
                    "Ready - Press 'q' to quit, 'u' to upload, 'd' to download", id="status-bar"
                )

    async def on_mount(self) -> None:
        """Handle app mount."""
        self.title = "TeleVault - Encrypted Cloud Storage"

        # Check authentication on mount
        await self._check_auth()

    async def _check_auth(self) -> None:
        """Check if user is authenticated."""
        try:
            self.vault_instance = TeleVault()
            await self.vault_instance.connect(skip_channel=True)

            if await self.vault_instance.is_authenticated():
                self.is_authenticated = True
                await self.vault_instance.disconnect()
                self.refresh(layout=True)
                await self._load_files()
            else:
                await self.vault_instance.disconnect()
                self.is_authenticated = False
                self.status_message = "Not logged in - Press 'l' to login"
        except Exception as e:
            self.status_message = f"Error: {str(e)}"
            self.is_authenticated = False

    async def _load_files(self) -> None:
        """Load files from vault."""
        if not self.is_authenticated:
            return

        try:
            self.status_message = "Loading files..."
            vault = TeleVault()
            await vault.connect()

            files = await vault.list_files()
            self.files = files

            # Update table
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

            # Update stats
            total_size = sum(f.size for f in files)
            self.query_one("#stat-files", Label).update(f"Files: {len(files)}")
            self.query_one("#stat-size", Label).update(f"Total Size: {format_size(total_size)}")

            await vault.disconnect()
            self.status_message = f"Loaded {len(files)} files"

        except Exception as e:
            self.status_message = f"Error loading files: {str(e)}"

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        """Handle button presses."""
        button_id = event.button.id

        if button_id == "btn-login":
            await self._do_login()
        elif button_id == "btn-exit":
            self.exit()
        elif button_id == "btn-upload":
            await self._do_upload()
        elif button_id == "btn-search":
            await self._do_search()
        elif button_id == "btn-refresh":
            await self._load_files()
        elif button_id == "btn-status":
            await self._show_status()
        elif button_id == "btn-whoami":
            await self._show_whoami()
        elif button_id == "btn-logout":
            await self._do_logout()

    async def _do_login(self) -> None:
        """Show login dialog."""
        self.push_screen(LoginScreen())

    async def _do_upload(self) -> None:
        """Show upload dialog."""
        if not self.is_authenticated:
            self.notify("Please login first", severity="error")
            return
        self.push_screen(UploadScreen())

    async def _do_search(self) -> None:
        """Focus search input."""
        if not self.is_authenticated:
            self.notify("Please login first", severity="error")
            return
        self.query_one("#search-input", Input).focus()

    async def _show_status(self) -> None:
        """Show vault status."""
        if not self.is_authenticated:
            self.notify("Please login first", severity="error")
            return

        try:
            vault = TeleVault()
            await vault.connect()
            status = await vault.get_status()
            await vault.disconnect()

            message = f"""
📊 Vault Status

Channel ID: {status["channel_id"]}
Files: {status["file_count"]}
Total Size: {format_size(status["total_size"])}
Stored Size: {format_size(status["stored_size"])}
Compression: {status["compression_ratio"]:.1%}
            """.strip()

            self.notify(message, title="Status", timeout=10)
        except Exception as e:
            self.notify(f"Error: {str(e)}", severity="error")

    async def _show_whoami(self) -> None:
        """Show current user info."""
        if not self.is_authenticated:
            self.notify("Please login first", severity="error")
            return

        try:
            vault = TeleVault()
            await vault.connect()
            me = await vault.telegram._client.get_me()
            await vault.disconnect()

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

    async def _do_logout(self) -> None:
        """Logout user."""
        config_dir = Path.home() / ".config" / "televault"
        telegram_config = config_dir / "telegram.json"

        if telegram_config.exists():
            telegram_config.unlink()
            self.is_authenticated = False
            self.files = []
            self.refresh(layout=True)
            self.notify("Logged out successfully", severity="information")
        else:
            self.notify("Not logged in", severity="warning")

    def action_refresh(self) -> None:
        """Refresh action."""
        asyncio.create_task(self._load_files())

    def action_upload(self) -> None:
        """Upload action."""
        asyncio.create_task(self._do_upload())

    def action_download(self) -> None:
        """Download action."""
        self.notify("Select a file and press Enter to download", severity="information")

    def action_delete(self) -> None:
        """Delete action."""
        self.notify("Select a file and press Delete to remove", severity="information")

    def action_search(self) -> None:
        """Search action."""
        asyncio.create_task(self._do_search())

    def action_login(self) -> None:
        """Login action."""
        asyncio.create_task(self._do_login())

    async def on_data_table_row_selected(self, event: DataTable.RowSelected) -> None:
        """Handle file selection."""
        if not self.files:
            return

        row_index = event.cursor_row
        if 0 <= row_index < len(self.files):
            self.selected_file = self.files[row_index]
            await self._download_file(self.selected_file)

    async def _download_file(self, file_metadata) -> None:
        """Download selected file."""
        self.push_screen(DownloadScreen(file_metadata))

    def watch_status_message(self, message: str) -> None:
        """Update status bar when message changes."""
        try:
            status_bar = self.query_one("#status-bar", Static)
            status_bar.update(message)
        except Exception:
            pass


class LoginScreen(Screen):
    """Login screen for authentication."""

    def compose(self) -> ComposeResult:
        with Container(classes="login-container"):
            yield Label("🔐 Login to Telegram", classes="title")
            yield Label("")
            yield Label("Enter your phone number:", classes="info-text")
            yield Input(placeholder="+1234567890", id="phone-input")
            yield Label("")
            yield Button("Send Code", id="btn-send-code", variant="primary")
            yield Button("Cancel", id="btn-cancel")

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-send-code":
            phone = self.query_one("#phone-input", Input).value
            if phone:
                await self._do_login(phone)
        elif event.button.id == "btn-cancel":
            self.app.pop_screen()

    async def _do_login(self, phone: str) -> None:
        """Perform login."""
        try:
            self.app.notify("Connecting to Telegram...")

            vault = TeleVault()
            await vault.connect(skip_channel=True)

            if await vault.is_authenticated():
                self.app.notify("Already logged in!")
                self.app.is_authenticated = True
                self.app.refresh(layout=True)
                await vault.disconnect()
                self.app.pop_screen()
                return

            # Show code input screen
            self.app.push_screen(CodeScreen(vault, phone))

        except Exception as e:
            self.app.notify(f"Login error: {str(e)}", severity="error")


class CodeScreen(Screen):
    """Screen for entering verification code."""

    def __init__(self, vault: TeleVault, phone: str):
        super().__init__()
        self.vault = vault
        self.phone = phone

    def compose(self) -> ComposeResult:
        with Container(classes="login-container"):
            yield Label("📱 Verification Code", classes="title")
            yield Label("")
            yield Label(f"Enter the code sent to {self.phone}:", classes="info-text")
            yield Input(placeholder="12345", id="code-input")
            yield Label("")
            yield Button("Verify", id="btn-verify", variant="primary")
            yield Button("Back", id="btn-back")

    async def on_button_pressed(self, event: Button.Pressed) -> None:
        if event.button.id == "btn-verify":
            code = self.query_one("#code-input", Input).value
            if code:
                await self._verify_code(code)
        elif event.button.id == "btn-back":
            await self.vault.disconnect()
            self.app.pop_screen()

    async def _verify_code(self, code: str) -> None:
        """Verify the code."""
        try:
            await self.vault.telegram._client.sign_in(self.phone, code)

            # Save session
            session_string = self.vault.telegram._client.session.save()
            from .telegram import TelegramConfig

            config = TelegramConfig.from_env()
            config.session_string = session_string
            config.save()

            self.app.notify("✓ Login successful!")
            self.app.is_authenticated = True
            self.app.refresh(layout=True)
            await self.vault.disconnect()
            self.app.pop_screen()
            self.app.pop_screen()  # Pop login screen too

        except Exception as e:
            self.app.notify(f"Verification failed: {str(e)}", severity="error")


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
        """Upload the file."""
        try:
            self.app.notify(f"Uploading {Path(path).name}...")

            vault = TeleVault(password=password)
            await vault.connect()

            metadata = await vault.upload(path)
            await vault.disconnect()

            self.app.notify(f"✓ Uploaded: {metadata.name}")
            self.app.pop_screen()

            # Refresh file list
            await self.app._load_files()

        except Exception as e:
            self.app.notify(f"Upload failed: {str(e)}", severity="error")


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
        """Download the file."""
        try:
            self.app.notify(f"Downloading {self.file_metadata.name}...")

            vault = TeleVault(password=password)
            await vault.connect()

            output_path = await vault.download(self.file_metadata.id, output_path=output)
            await vault.disconnect()

            self.app.notify(f"✓ Downloaded to: {output_path}")
            self.app.pop_screen()

        except Exception as e:
            self.app.notify(f"Download failed: {str(e)}", severity="error")


def run_tui():
    """Run the TUI application."""
    app = VaultApp()
    app.run()


if __name__ == "__main__":
    run_tui()
