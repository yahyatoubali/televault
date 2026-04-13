"""TeleVault CLI - Command line interface."""

import asyncio
import contextlib
import signal
import sys
import time
from pathlib import Path

import click
from rich.console import Console
from rich.progress import BarColumn, Progress, SpinnerColumn, TextColumn, TimeRemainingColumn
from rich.table import Table

from .config import Config, get_config_dir
from .core import DownloadProgress, TeleVault, UploadProgress
from .logging import setup_logging

console = Console()


def format_size(size: int) -> str:
    """Format bytes as human readable."""
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if size < 1024:
            return f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} PB"


def format_speed(bytes_per_sec: float) -> str:
    """Format transfer speed as human readable."""
    if bytes_per_sec <= 0:
        return ""
    for unit in ["B/s", "KB/s", "MB/s", "GB/s"]:
        if bytes_per_sec < 1024:
            return f"{bytes_per_sec:.1f} {unit}"
        bytes_per_sec /= 1024
    return f"{bytes_per_sec:.1f} TB/s"


PHASE_LABELS = {
    "uploading": {
        "hashing": "Hashing",
        "metadata": "Sending metadata",
        "uploading": "Uploading",
        "index": "Saving index",
        "done": "Done",
    },
    "downloading": {
        "metadata": "Fetching metadata",
        "downloading": "Downloading",
        "verifying": "Verifying",
        "done": "Done",
    },
}


def check_api_credentials_cli() -> bool:
    """Check if Telegram API credentials are configured."""
    import json
    import os

    # Check environment variables
    if os.environ.get("TELEGRAM_API_ID") and os.environ.get("TELEGRAM_API_HASH"):
        return True

    # Check config file
    config_path = get_config_dir() / "telegram.json"
    if config_path.exists():
        try:
            with open(config_path) as f:
                data = json.load(f)
                if data.get("api_id") and data.get("api_hash"):
                    return True
        except Exception:
            pass

    return False


def show_api_credentials_error():
    """Show error message for missing API credentials."""
    console.print("[bold red]✗ Telegram API credentials not configured![/bold red]\n")
    console.print("You need to set up your Telegram API credentials before logging in.")
    console.print("\n[bold]How to get your API credentials:[/bold]")
    console.print("1. Visit: https://my.telegram.org")
    console.print("2. Log in with your phone number")
    console.print("3. Go to 'API development tools'")
    console.print("4. Create a new application")
    console.print("5. Note your 'api_id' and 'api_hash'")
    console.print("\n[bold]Then set them up using one of these methods:[/bold]\n")

    console.print("[bold]Method 1 - Environment variables (recommended):[/bold]")
    console.print("  export TELEGRAM_API_ID=your_api_id")
    console.print("  export TELEGRAM_API_HASH=your_api_hash")
    console.print("\n[bold]Method 2 - Config file:[/bold]")
    config_path = get_config_dir() / "telegram.json"
    console.print(f"  Edit: {config_path}")
    console.print('  Add: {"api_id": 12345, "api_hash": "your_hash_here"}')


def run_async(coro):
    """Run async function with friendly error handling."""
    try:
        loop = asyncio.get_running_loop()
    except RuntimeError:
        loop = None

    try:
        if loop and loop.is_running():
            import concurrent.futures

            with concurrent.futures.ThreadPoolExecutor() as executor:
                future = executor.submit(asyncio.run, coro)
                return future.result()
        else:
            return asyncio.run(coro)
    except ConnectionError as e:
        console.print(f"[red]Connection error: {e}[/red]")
        console.print("[dim]Check your internet connection and try again.[/dim]")
        sys.exit(1)
    except RuntimeError as e:
        msg = str(e)
        if "Not connected" in msg:
            console.print("[red]Not connected to Telegram.[/red]")
            console.print("[dim]Run 'tvt login' first.[/dim]")
        else:
            console.print(f"[red]Error: {e}[/red]")
        sys.exit(1)
    except FileNotFoundError as e:
        console.print(f"[red]Error: {e}[/red]")
        sys.exit(1)
    except KeyboardInterrupt:
        console.print("\n[yellow]Interrupted.[/yellow]")
        sys.exit(130)
    except Exception as e:
        error_type = type(e).__name__
        if "FloodWait" in error_type:
            console.print(f"[red]Telegram rate limit: {e}[/red]")
            console.print("[dim]Wait a few minutes and try again.[/dim]")
        elif "AuthKey" in error_type or "Unauthorized" in error_type:
            console.print("[red]Session expired or invalid.[/red]")
            console.print("[dim]Run 'tvt login' to re-authenticate.[/dim]")
        elif "ApiId" in error_type:
            console.print("[red]Invalid Telegram API credentials.[/red]")
            console.print("[dim]Check your API_ID and API_HASH at https://my.telegram.org[/dim]")
        else:
            console.print(f"[red]Error ({error_type}): {e}[/red]")
            console.print("[dim]Run with --debug for more details.[/dim]")
        sys.exit(1)


async def check_auth(vault: TeleVault) -> bool:
    """Check if user is authenticated. Returns True if authenticated, False otherwise."""
    if not await vault.is_authenticated():
        console.print("[red]Not logged in.[/red]")
        console.print("[dim]Run 'tvt login' to authenticate.[/dim]")
        return False
    return True


async def check_channel(vault: TeleVault) -> bool:
    """Check if channel is set. Returns True if set, False otherwise."""
    if vault.config.channel_id is None:
        console.print("[red]No storage channel configured.[/red]")
        console.print("[dim]Run 'tvt setup' to create or select a channel.[/dim]")
        return False
    return True


@click.group(invoke_without_command=True)
@click.option("-v", "--verbose", is_flag=True, help="Enable verbose logging.")
@click.option("--debug", is_flag=True, help="Enable debug logging.")
@click.version_option(version=__import__("televault").__version__, prog_name="tvt")
@click.pass_context
def main(ctx, verbose, debug):
    """TeleVault - Unlimited cloud storage using Telegram.

    Short name: tvt (alias for televault)

    Common commands:

      tvt push <file>       Upload a file

      tvt pull <file>       Download a file

      tvt ls                List files

      tvt cat <file>        Stream file to stdout

      tvt preview <file>    Show file preview

      tvt stat              Show vault statistics

      tvt setup              Configure storage channel

      tvt channel            Show channel info

    Pipe examples:

      cat secret.txt | tvt push - --name secret.txt

      tvt cat photo.jpg > photo.jpg

      tvt ls --json | jq '.[].name'
    """
    if debug:
        setup_logging("DEBUG")
    elif verbose:
        setup_logging("INFO")
    else:
        setup_logging("WARNING")

    with contextlib.suppress(AttributeError, OSError):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)

    ctx.ensure_object(dict)

    if ctx.invoked_subcommand is None:
        click.echo(ctx.get_help())


@main.command()
@click.option("--phone", "-p", help="Phone number for login")
def login(phone: str | None):
    """Login to Telegram."""

    # Check API credentials first
    if not check_api_credentials_cli():
        show_api_credentials_error()
        sys.exit(1)

    async def _login():
        vault = TeleVault()
        await vault.connect(skip_channel=True)  # Don't try to access channel yet

        console.print("[bold blue]TeleVault Login[/bold blue]")
        console.print("You'll receive a code on Telegram.\n")

        await vault.login(phone)

        console.print("\n[bold green]✓ Login successful![/bold green]")
        console.print(f"Session saved to: {get_config_dir() / 'telegram.json'}")

        # Now set up channel if configured
        if vault.config.channel_id:
            await vault.telegram.set_channel(vault.config.channel_id)
            console.print(f"Channel configured: {vault.config.channel_id}")

        await vault.disconnect()

    run_async(_login())


@main.command()
def logout():
    """Logout and clear session."""
    config_dir = get_config_dir()
    telegram_config = config_dir / "telegram.json"

    if telegram_config.exists():
        telegram_config.unlink()
        console.print("[green]✓ Logged out successfully[/green]")
    else:
        console.print("[yellow]Not logged in[/yellow]")


@main.command()
@click.option("--channel-id", "-c", type=int, help="Use an existing channel by ID")
@click.option("--auto", "-a", is_flag=True, help="Auto-create a new channel")
def setup(channel_id: int | None, auto: bool):
    """Set up or change storage channel.

    \b
    Examples:
      tvt setup              Interactive setup (recommended)
      tvt setup --auto       Auto-create a new channel
      tvt setup -c -100xxx  Use an existing channel ID
    """

    async def _setup():
        vault = TeleVault()
        await vault.connect(skip_channel=True)

        if not await check_auth(vault):
            await vault.disconnect()
            return

        me = await vault.get_account_info()
        name = f"{me.get('first_name', '')} {me.get('last_name', '')}".strip()
        console.print(
            f"[bold]Account:[/bold] {name}"
            + (f" (@{me['username']})" if me.get("username") else "")
        )
        console.print()

        if vault.config.channel_id:
            console.print(f"[dim]Current channel: {vault.config.channel_id}[/dim]")
            try:
                info = await vault.test_channel(vault.config.channel_id)
                if info["accessible"]:
                    status = "[green]connected[/green]"
                    if info.get("title"):
                        console.print(f"[dim]  Name: {info['title']}[/dim]")
                    if not info["writable"]:
                        status = "[yellow]read-only[/yellow]"
                    console.print(f"[dim]  Status: {status}[/dim]")
                else:
                    console.print("[dim]  Status: [red]not accessible[/red][/dim]")
            except Exception:
                console.print("[dim]  Status: [yellow]could not verify[/yellow][/dim]")
            console.print()

        if channel_id:
            console.print(f"[bold]Validating channel {channel_id}...[/bold]")
            try:
                info = await vault.test_channel(channel_id)
            except Exception as e:
                console.print(f"[red]Could not access channel: {e}[/red]")
                await vault.disconnect()
                return

            if not info["accessible"]:
                console.print(
                    "[red]Channel not accessible. Check the ID and your permissions.[/red]"
                )
                await vault.disconnect()
                return

            console.print(f"  Title: {info.get('title', 'Unknown')}")
            if info.get("username"):
                console.print(f"  Username: @{info['username']}")
            console.print(
                f"  Writable: {'Yes' if info['writable'] else '[red]No - you need admin rights[/red]'}"
            )

            if not info["writable"]:
                console.print("[red]Channel is not writable. You need admin rights.[/red]")
                await vault.disconnect()
                return

            cid = await vault.setup_channel(channel_id)
            console.print(f"\n[green]Channel {cid} configured successfully![/green]")
            console.print(f"[dim]You can change it anytime with: tvt channel switch[/dim]")
        elif auto:
            console.print("[bold]Creating new private channel...[/bold]")
            cid = await vault.setup_channel()
            console.print(f"[green]Created channel: {cid}[/green]")

            info = await vault.test_channel(cid)
            if info["writable"]:
                console.print("[green]Test message sent and verified.[/green]")
            console.print(f"[dim]You can change it anytime with: tvt channel switch[/dim]")
        else:
            console.print("[bold blue]TeleVault Channel Setup[/bold blue]\n")
            console.print("How would you like to set up storage?\n")
            console.print("  [bold]1.[/bold] Create a new private channel (recommended)")
            console.print("  [bold]2.[/bold] Use an existing channel by ID")
            console.print("  [bold]3.[/bold] Pick from your channels\n")

            choice = click.prompt("Enter choice", type=str, default="1").strip()

            if choice == "1":
                console.print("\n[bold]Creating new storage channel...[/bold]")
                cid = await vault.setup_channel()

                info = await vault.test_channel(cid)
                if info["writable"]:
                    console.print(f"[green]Created and verified channel: {cid}[/green]")
                    if info.get("title"):
                        console.print(f"[dim]  Name: {info['title']}[/dim]")
                    console.print("[green]Test message sent and verified.[/green]")
                else:
                    console.print(
                        f"[yellow]Created channel: {cid}, but could not verify write access.[/yellow]"
                    )
                console.print(f"[dim]You can change it anytime with: tvt channel switch[/dim]")

            elif choice == "2":
                console.print("\n[bold]Use existing channel[/bold]")
                console.print("[dim]The channel ID starts with -100 (e.g., -1001234567890)[/dim]")
                console.print("[dim]You can find it in the channel info or use option 3.[/dim]\n")

                try:
                    existing_id = click.prompt("Enter channel ID", type=str).strip()
                    existing_id_int = int(existing_id)
                except (ValueError, EOFError):
                    console.print("[red]Invalid channel ID.[/red]")
                    await vault.disconnect()
                    return

                console.print(f"\n[bold]Validating channel {existing_id_int}...[/bold]")
                try:
                    info = await vault.test_channel(existing_id_int)
                except Exception as e:
                    console.print(f"[red]Could not access channel: {e}[/red]")
                    await vault.disconnect()
                    return

                if not info["accessible"]:
                    console.print("[red]Channel not found or you don't have access.[/red]")
                    await vault.disconnect()
                    return

                console.print(f"  Title: {info.get('title', 'Unknown')}")
                console.print(f"  Writable: {'Yes' if info['writable'] else 'No'}")

                if not info["writable"]:
                    console.print("[red]Channel is not writable. You need admin rights.[/red]")
                    await vault.disconnect()
                    return

                cid = await vault.setup_channel(existing_id_int)
                console.print(f"\n[green]Channel {cid} configured successfully![/green]")

            elif choice == "3":
                console.print("\n[bold]Loading your channels...[/bold]\n")
                try:
                    channels = await vault.list_channels()
                except Exception as e:
                    console.print(f"[red]Could not list channels: {e}[/red]")
                    await vault.disconnect()
                    return

                if not channels:
                    console.print("[yellow]No channels found. Create one instead.[/yellow]")
                    await vault.disconnect()
                    return

                current_id = vault.config.channel_id
                for i, ch in enumerate(channels, 1):
                    marker = " [dim](current)[/dim]" if ch["id"] == current_id else ""
                    admin = " [green]admin[/green]" if ch.get("is_admin") else ""
                    console.print(f"  [bold]{i}.[/bold] {ch['title']}{admin}{marker}")
                    console.print(f"     ID: {ch['id']}")

                console.print()
                selection = click.prompt("Select channel number", type=int)
                if 1 <= selection <= len(channels):
                    selected = channels[selection - 1]

                    console.print(f"\n[bold]Validating {selected['title']}...[/bold]")
                    info = await vault.test_channel(selected["id"])
                    console.print(f"  Writable: {'Yes' if info['writable'] else 'No'}")

                    if not info["writable"]:
                        console.print("[red]Not writable. You need admin rights.[/red]")
                        await vault.disconnect()
                        return

                    cid = await vault.setup_channel(selected["id"])
                    console.print(
                        f"\n[green]Switched to channel {cid} ({selected['title']})[/green]"
                    )
                else:
                    console.print("[red]Invalid selection.[/red]")
            else:
                console.print("[red]Invalid choice.[/red]")

        await vault.disconnect()

    run_async(_setup())


@main.command()
def channel():
    """Show current storage channel info.

    \b
    Examples:
      tvt channel          Show current channel info
      tvt channel switch   Switch to a different channel
    """

    async def _channel():
        vault = TeleVault()

        try:
            await vault.connect(skip_channel=True)
        except Exception as e:
            console.print(f"[red]Connection error: {e}[/red]")
            console.print("[dim]Check your internet connection.[/dim]")
            return

        try:
            if not await check_auth(vault):
                await vault.disconnect()
                return

            config = vault.config
            if not config.channel_id:
                console.print("[yellow]No storage channel configured.[/yellow]")
                console.print("[dim]Run 'tvt setup' to configure one.[/dim]")
                await vault.disconnect()
                return

            console.print(f"[bold]Current Channel[/bold]\n")
            console.print(f"  ID: [cyan]{config.channel_id}[/cyan]")

            console.print("\n[bold]Validating channel...[/bold]")
            try:
                info = await vault.test_channel(config.channel_id)
                if info["accessible"]:
                    console.print(f"  Name: {info.get('title', 'Unknown')}")
                    if info.get("username"):
                        console.print(f"  Username: @{info['username']}")
                    console.print(f"  Writable: {'Yes' if info['writable'] else '[red]No[/red]'}")
                    if info.get("member_count") is not None:
                        console.print(f"  Members: {info['member_count']}")
                    console.print(f"  Status: [green]connected[/green]")
                else:
                    console.print("  Status: [red]not accessible[/red]")
                    console.print(
                        "[dim]The channel may have been deleted or you lost access.[/dim]"
                    )
                    console.print("[dim]Run 'tvt setup' to configure a new channel.[/dim]")
            except Exception as e:
                console.print(f"  Status: [yellow]could not verify: {e}[/yellow]")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
        finally:
            await vault.disconnect()

    run_async(_channel())


@main.command()
@click.argument("file_path", type=click.Path(exists=False))
@click.option("--password", "-p", help="Encryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--no-compress", is_flag=True, help="Disable compression")
@click.option("--no-encrypt", is_flag=True, help="Disable encryption")
@click.option("--recursive", "-r", is_flag=True, help="Upload directory recursively")
@click.option("--resume", is_flag=True, help="Resume interrupted upload")
@click.option("--name", "-n", help="Filename when reading from stdin (e.g., -)")
def push(
    file_path: str,
    password: str | None,
    no_compress: bool,
    no_encrypt: bool,
    recursive: bool,
    resume: bool,
    name: str | None,
):
    """Upload a file or directory to TeleVault.

    Use '-' as FILE_PATH to read from stdin:

      cat data.json | tvt push - --name data.json

    """

    async def _push():
        config = Config.load_or_create()

        if no_compress:
            config.compression = False
        if no_encrypt:
            config.encryption = False

        if config.encryption and not password:
            console.print("[yellow]Warning: Encryption enabled but no password provided.[/yellow]")
            console.print("Set password with --password or TELEVAULT_PASSWORD env var.")
            console.print("Use --no-encrypt to disable encryption.")
            console.print("[dim]File will be uploaded WITHOUT encryption.[/dim]\n")

        vault = TeleVault(config=config, password=password)
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        # Handle stdin upload (pipe: cat file | tvt push - --name file)
        if file_path == "-":
            if not name:
                console.print("[red]Error: --name is required when reading from stdin[/red]")
                console.print("[dim]Example: cat data.json | tvt push - --name data.json[/dim]")
                await vault.disconnect()
                return

            import sys

            data = sys.stdin.buffer.read()
            if not data:
                console.print("[red]Error: No data read from stdin[/red]")
                await vault.disconnect()
                return

            with Progress(
                SpinnerColumn(),
                TextColumn("[progress.description]{task.description}"),
                console=console,
            ) as progress:
                progress.add_task(f"Uploading {name}", total=None)
                metadata = await vault.upload_stream(
                    data=data,
                    filename=name,
                    password=password,
                )

            console.print(
                f"\n[bold green]Uploaded {name}[/bold green] ({format_size(metadata.size)})"
            )
            console.print(f"  File ID: {metadata.id}")

            # Update file cache for completion
            from .completion import save_file_cache

            save_file_cache([{"id": metadata.id, "name": metadata.name, "size": metadata.size}])

            await vault.disconnect()
            return

        file_path_obj = Path(file_path)

        if not file_path_obj.exists():
            console.print(f"[red]Error: '{file_path}' not found[/red]")
            await vault.disconnect()
            return

        # Handle directory upload
        if file_path_obj.is_dir():
            if not recursive:
                console.print(
                    f"[red]'{file_path}' is a directory. Use --recursive (-r) to upload.[/red]"
                )
                await vault.disconnect()
                return

            files = list(file_path_obj.rglob("*"))
            files = [f for f in files if f.is_file()]

            if not files:
                console.print("[yellow]No files found in directory.[/yellow]")
                await vault.disconnect()
                return

            console.print(f"[bold]Uploading {len(files)} files from {file_path_obj.name}/[/bold]\n")

            for i, f in enumerate(files, 1):
                rel_path = f.relative_to(file_path_obj)
                console.print(f"[{i}/{len(files)}] {rel_path}...", end=" ")
                try:
                    metadata = await vault.upload(f)
                    console.print(f"[green]✓[/green] ({format_size(metadata.size)})")
                except Exception as e:
                    console.print(f"[red]✗ {e}[/red]")

            console.print(f"\n[bold green]✓ Uploaded {len(files)} files[/bold green]")
        else:
            # Single file upload with progress

            with Progress(
                SpinnerColumn(),
                TextColumn("[progress.description]{task.description}"),
                BarColumn(),
                TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
                TextColumn("{task.fields[speed]}"),
                TimeRemainingColumn(),
                console=console,
                refresh_per_second=10,
            ) as progress:
                task = progress.add_task(
                    f"Hashing {file_path_obj.name}",
                    total=100,
                    speed="",
                )

                start_time = time.monotonic()
                last_size = 0

                def on_progress(p: UploadProgress):
                    nonlocal last_size, start_time
                    phase_label = PHASE_LABELS["uploading"].get(p.phase, p.phase)

                    elapsed = time.monotonic() - start_time
                    if p.phase == "uploading" and elapsed > 0:
                        speed = (p.uploaded_size - last_size) / max(elapsed, 0.001)
                        speed_str = format_speed(speed)
                    elif p.phase in ("hashing", "metadata", "index"):
                        speed_str = ""
                    else:
                        speed_str = ""

                    if p.phase in ("hashing", "metadata", "index"):
                        progress.update(
                            task,
                            description=f"{phase_label} {file_path_obj.name}",
                            completed=p.percent,
                            speed=speed_str,
                        )
                    else:
                        progress.update(
                            task,
                            description=f"{phase_label} {file_path_obj.name}",
                            completed=p.percent,
                            speed=speed_str,
                        )

                    if p.phase == "uploading":
                        start_time = time.monotonic()
                        last_size = p.uploaded_size

                if resume:
                    metadata = await vault.upload_resume(file_path, progress_callback=on_progress)
                    if len(metadata.chunks) < metadata.chunk_count:
                        console.print(
                            f"[dim]Resumed upload: {len(metadata.chunks)}"
                            f"/{metadata.chunk_count} chunks completed[/dim]"
                        )
                else:
                    metadata = await vault.upload(file_path, progress_callback=on_progress)
                progress.update(task, completed=100)  # Ensure 100% at end

            console.print("\n[bold green]✓ Uploaded successfully![/bold green]")
            console.print(f"  File ID: {metadata.id}")
            console.print(f"  Size: {format_size(metadata.size)}")
            console.print(f"  Chunks: {metadata.chunk_count}")
            console.print(f"  Encrypted: {'Yes' if metadata.encrypted else 'No'}")
            console.print(f"  Compressed: {'Yes' if metadata.compressed else 'No'}")

        await vault.disconnect()

    run_async(_push())


@main.command()
@click.argument("file_id_or_name")
@click.option("--output", "-o", type=click.Path(), help="Output path (use '-' for stdout)")
@click.option("--password", "-p", help="Decryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--resume", is_flag=True, help="Resume interrupted download")
def pull(file_id_or_name: str, output: str | None, password: str | None, resume: bool):
    """Download a file from TeleVault.

    Use '-o -' to write to stdout (for piping):

      tvt pull video.mp4 -o - > video.mp4

    """

    async def _pull():
        vault = TeleVault(password=password)
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        # Handle stdout output (pipe mode)
        if output == "-":
            import sys

            try:
                total = await vault.stream(
                    file_id_or_name,
                    output=sys.stdout.buffer,
                    password=password,
                )
                print(f"\n[{format_size(total)} downloaded]", file=sys.stderr)
            except FileNotFoundError:
                print(f"Error: File not found: {file_id_or_name}", file=sys.stderr)
                await vault.disconnect()
                sys.exit(1)
            except (ValueError, RuntimeError) as e:
                print(f"Error: {e}", file=sys.stderr)
                await vault.disconnect()
                sys.exit(1)
            except BrokenPipeError:
                await vault.disconnect()
                sys.exit(0)
            finally:
                await vault.disconnect()
            return

        with Progress(
            SpinnerColumn(),
            TextColumn("[progress.description]{task.description}"),
            BarColumn(),
            TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
            TextColumn("{task.fields[speed]}"),
            TimeRemainingColumn(),
            console=console,
            refresh_per_second=10,
        ) as progress:
            task = progress.add_task(
                f"Fetching {file_id_or_name}",
                total=100,
                speed="",
            )

            start_time = time.monotonic()
            last_size = 0

            def on_progress(p: DownloadProgress):
                nonlocal last_size, start_time
                phase_label = PHASE_LABELS["downloading"].get(p.phase, p.phase)

                elapsed = time.monotonic() - start_time
                if p.phase == "downloading" and elapsed > 0 and p.downloaded_size > 0:
                    speed = p.downloaded_size / max(elapsed, 0.001)
                    speed_str = format_speed(speed)
                elif p.phase in ("metadata", "verifying"):
                    speed_str = ""
                else:
                    speed_str = ""

                progress.update(
                    task,
                    description=f"{phase_label} {p.file_name}",
                    completed=p.percent,
                    speed=speed_str,
                )

            try:
                if resume:
                    output_path = await vault.download_resume(
                        file_id_or_name,
                        output_path=output,
                        progress_callback=on_progress,
                    )
                else:
                    output_path = await vault.download(
                        file_id_or_name,
                        output_path=output,
                        progress_callback=on_progress,
                    )
                progress.update(task, completed=100)  # Ensure 100% at end
            except FileNotFoundError:
                console.print(f"[red]✗ File not found: {file_id_or_name}[/red]")
                await vault.disconnect()
                sys.exit(1)
            except ValueError as e:
                console.print(f"[red]✗ Error: {e}[/red]")
                await vault.disconnect()
                sys.exit(1)

        console.print(f"\n[bold green]✓ Downloaded to: {output_path}[/bold green]")

        await vault.disconnect()

    run_async(_pull())


@main.command(name="ls")
@click.option("--json", "as_json", is_flag=True, help="Output as JSON")
@click.option("--sort", type=click.Choice(["name", "size", "date"]), default="name")
def list_files(as_json: bool, sort: str):
    """List all files in the vault."""

    async def _list():
        vault = TeleVault()
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        files = await vault.list_files()

        # Sort
        if sort == "name":
            files.sort(key=lambda f: f.name.lower())
        elif sort == "size":
            files.sort(key=lambda f: f.size, reverse=True)
        elif sort == "date":
            files.sort(key=lambda f: f.created_at, reverse=True)

        if as_json:
            import json

            output = [
                {"id": f.id, "name": f.name, "size": f.size, "encrypted": f.encrypted}
                for f in files
            ]
            click.echo(json.dumps(output, indent=2))
        else:
            if not files:
                console.print("[dim]No files in vault[/dim]")
            else:
                table = Table(title="TeleVault Files")
                table.add_column("ID", style="dim")
                table.add_column("Name")
                table.add_column("Size", justify="right")
                table.add_column("Chunks", justify="right")
                table.add_column("Encrypted")

                for f in files:
                    table.add_row(
                        f.id[:8],
                        f.name,
                        format_size(f.size),
                        str(f.chunk_count),
                        "🔒" if f.encrypted else "📄",
                    )

                console.print(table)
                total_size = format_size(sum(f.size for f in files))
                console.print(f"\n[dim]{len(files)} file(s), {total_size} total[/dim]")

        await vault.disconnect()

    run_async(_list())


@main.command()
@click.argument("query")
@click.option("--json", "as_json", is_flag=True, help="Output as JSON")
def search(query: str, as_json: bool):
    """Search files by name (alias: find)."""

    async def _search():
        vault = TeleVault()
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        files = await vault.search(query)

        if as_json:
            import json

            output = [
                {"id": f.id, "name": f.name, "size": f.size, "encrypted": f.encrypted}
                for f in files
            ]
            click.echo(json.dumps(output, indent=2))
        else:
            if not files:
                console.print(f"[dim]No files matching '{query}'[/dim]")
            else:
                for f in files:
                    console.print(f"[cyan]{f.id[:8]}[/cyan] {f.name} ({format_size(f.size)})")

        # Update file cache for completion
        from .completion import save_file_cache

        save_file_cache([{"id": f.id, "name": f.name, "size": f.size} for f in files])

        await vault.disconnect()

    run_async(_search())


@main.command()
@click.argument("file_id_or_name")
@click.option("--json", "as_json", is_flag=True, help="Output as JSON")
def info(file_id_or_name: str, as_json: bool):
    """Show detailed file information."""

    async def _info():
        vault = TeleVault()
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        try:
            # Find file
            files = await vault.search(file_id_or_name)
            if not files:
                # Try by ID
                index = await vault.telegram.get_index()
                for fid, msg_id in index.files.items():
                    if fid.startswith(file_id_or_name):
                        metadata = await vault.telegram.get_metadata(msg_id)
                        files = [metadata]
                        break

            if not files:
                console.print(f"[red]File not found: {file_id_or_name}[/red]")
                await vault.disconnect()
                return

            f = files[0]

            if as_json:
                import json
                from datetime import datetime

                output = {
                    "id": f.id,
                    "name": f.name,
                    "size": f.size,
                    "hash": f.hash,
                    "chunks": f.chunk_count,
                    "encrypted": f.encrypted,
                    "compressed": f.compressed,
                    "compression_ratio": f.compression_ratio if f.compressed else None,
                    "mime_type": f.mime_type,
                    "created_at": f.created_at,
                    "created_at_iso": datetime.fromtimestamp(f.created_at).isoformat()
                    if f.created_at
                    else None,
                    "stored_size": sum(c.size for c in f.chunks) if f.chunks else None,
                }
                click.echo(json.dumps(output, indent=2))
            else:
                console.print(f"[bold]{f.name}[/bold]\n")
                console.print(f"  ID:          {f.id}")
                console.print(f"  Size:        {format_size(f.size)}")
                console.print(f"  Hash:        {f.hash}")
                console.print(f"  Chunks:      {f.chunk_count}")
                console.print(f"  Encrypted:   {'Yes' if f.encrypted else 'No'}")
                console.print(f"  Compressed:  {'Yes' if f.compressed else 'No'}")
                if f.compressed and f.compression_ratio:
                    console.print(f"  Comp. ratio: {f.compression_ratio:.1%}")
                if f.mime_type:
                    console.print(f"  MIME type:   {f.mime_type}")

                from datetime import datetime

                if f.created_at:
                    created = datetime.fromtimestamp(f.created_at)
                    console.print(f"  Created:     {created.strftime('%Y-%m-%d %H:%M')}")

                if f.chunks:
                    stored = sum(c.size for c in f.chunks)
                    console.print(f"  Stored size: {format_size(stored)}")

        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")

        await vault.disconnect()

    run_async(_info())


@main.command()
@click.argument("file_id_or_name")
@click.option("--yes", "-y", is_flag=True, help="Skip confirmation")
def rm(file_id_or_name: str, yes: bool):
    """Delete a file from the vault."""

    async def _rm():
        vault = TeleVault()
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        if not yes and not click.confirm(f"Delete '{file_id_or_name}'?"):
            console.print("[dim]Cancelled[/dim]")
            await vault.disconnect()
            return

        deleted = await vault.delete(file_id_or_name)

        if deleted:
            console.print(f"[green]✓ Deleted: {file_id_or_name}[/green]")
        else:
            console.print(f"[red]✗ File not found: {file_id_or_name}[/red]")

        await vault.disconnect()

    run_async(_rm())


@main.command()
def whoami():
    """Show current Telegram account."""

    async def _whoami():
        vault = TeleVault()
        try:
            await vault.connect()

            if not await check_auth(vault):
                return

            me = await vault.telegram._client.get_me()

            if me is None:
                console.print("[red]Not logged in. Run 'televault login' first.[/red]")
                return

            console.print(f"[bold]{me.first_name}[/bold]", end="")
            if me.last_name:
                console.print(f" {me.last_name}", end="")
            console.print()

            if me.username:
                console.print(f"  @{me.username}")
            console.print(f"  ID: {me.id}")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
        finally:
            await vault.disconnect()

    run_async(_whoami())


@main.command()
def tui():
    """Launch the interactive TUI (BETA).

    The TUI is experimental and may have stability issues.
    For reliable operations, use the CLI commands directly:
    tvt push, tvt pull, tvt ls, tvt rm, etc.
    """
    console.print(
        "[yellow]⚠ TUI is in BETA — for best reliability use CLI commands directly[/yellow]"
    )
    try:
        from .tui import run_tui

        run_tui()
    except ImportError as e:
        if "textual" in str(e).lower():
            console.print("[red]Error: The TUI requires the 'textual' package.[/red]")
            console.print("[dim]Install it with: pip install televault[/dim]")
        else:
            console.print(f"[red]Error: {e}[/red]")
        sys.exit(1)
    except Exception as e:
        try:
            sys.stdout.write("\033[?25h\033[0m\033[2J\033[H")
            sys.stdout.flush()
        except Exception:
            pass
        console.print(f"[red]TUI error: {e}[/red]")
        console.print("[dim]If the terminal looks broken, run: reset[/dim]")
        sys.exit(1)


@main.command(name="gc")
@click.option(
    "--force", is_flag=True, help="Actually delete orphaned messages (default is dry-run)"
)
@click.option("--clean-partials", is_flag=True, help="Also remove incomplete uploads")
def garbage_collect(force: bool, clean_partials: bool):
    """Find and remove orphaned messages from the vault.

    By default runs in dry-run mode (no deletions). Use --force to actually delete.
    """

    async def _gc():
        from .gc import cleanup_partial_uploads, collect_garbage

        vault = TeleVault()
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        if clean_partials:
            console.print("[bold]Cleaning up partial uploads...[/bold]")
            cleaned = await cleanup_partial_uploads(vault.telegram)
            console.print(f"  Removed {cleaned} incomplete uploads")

        dry_run = not force
        if dry_run:
            console.print(
                "[yellow]Dry run - no messages will be deleted. Use --force to delete.[/yellow]"
            )

        console.print("[bold]Scanning for orphaned messages...[/bold]")
        result = await collect_garbage(vault.telegram, dry_run=dry_run)

        if not result["orphaned_messages"]:
            console.print("[green]No orphaned messages found. Vault is clean![/green]")
        else:
            count = len(result["orphaned_messages"])
            size = result["orphaned_size"]
            console.print(f"  Found {count} orphaned messages ({format_size(size)})")

            if dry_run:
                for msg in result["orphaned_messages"][:10]:
                    console.print(
                        f"  - Message {msg['id']} ({msg['type']}, {format_size(msg['size'])})"
                    )
                if count > 10:
                    console.print(f"  ... and {count - 10} more")
            else:
                deleted = result["deleted_count"]
                console.print(f"[green]Deleted {deleted} orphaned messages[/green]")

        await vault.disconnect()

    run_async(_gc())


@main.command()
@click.argument("file_id_or_name")
@click.option("--password", "-p", help="Decryption password", envvar="TELEVAULT_PASSWORD")
def verify(file_id_or_name: str, password: str | None):
    """Verify a file's integrity by re-downloading and checking all hashes."""

    async def _verify():
        vault = TeleVault(password=password)
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        from .chunker import hash_data as calc_hash

        index = await vault.telegram.get_index()

        if file_id_or_name in index.files:
            metadata = await vault.telegram.get_metadata(index.files[file_id_or_name])
        else:
            files = await vault.list_files()
            matches = [f for f in files if f.name == file_id_or_name or file_id_or_name in f.name]
            if not matches:
                console.print(f"[red]File not found: {file_id_or_name}[/red]")
                await vault.disconnect()
                sys.exit(1)
            if len(matches) > 1:
                console.print(f"[red]Multiple files match '{file_id_or_name}'[/red]")
                await vault.disconnect()
                sys.exit(1)
            metadata = await vault.telegram.get_metadata(matches[0].message_id)

        console.print(f"[bold]Verifying: {metadata.name}[/bold]")
        console.print(f"  Size: {format_size(metadata.size)}")
        console.print(f"  Chunks: {metadata.chunk_count}")
        console.print(f"  Encrypted: {'Yes' if metadata.encrypted else 'No'}")
        console.print(f"  Compressed: {'Yes' if metadata.compressed else 'No'}")

        errors = 0
        for i, chunk in enumerate(metadata.chunks, 1):
            try:
                data = await vault.telegram.download_chunk(chunk.message_id)

                stored_hash = chunk.hash
                actual_hash = calc_hash(data)

                if stored_hash == actual_hash:
                    console.print(f"  [green]✓[/green] Chunk {i}/{metadata.chunk_count} OK")
                else:
                    console.print(f"  [red]✗[/red] Chunk {i}/{metadata.chunk_count} HASH MISMATCH")
                    errors += 1

                if chunk.original_hash:
                    pwd = password
                    if metadata.encrypted and pwd:
                        from .crypto import decrypt_chunk

                        original_data = decrypt_chunk(data, pwd)
                        if metadata.compressed:
                            from .compress import decompress_data

                            original_data = decompress_data(original_data)
                        original_hash = calc_hash(original_data)
                        if original_hash == chunk.original_hash:
                            console.print("    [green]✓[/green] Decrypted content verified")
                        else:
                            console.print("    [red]✗[/red] Decrypted content HASH MISMATCH")
                            errors += 1

            except Exception as e:
                console.print(f"  [red]✗[/red] Chunk {i}/{metadata.chunk_count} ERROR: {e}")
                errors += 1

        if errors == 0:
            console.print("\n[bold green]All chunks verified successfully![/bold green]")
        else:
            console.print(f"\n[bold red]{errors} chunk(s) failed verification![/bold red]")

        await vault.disconnect()

    run_async(_verify())


@main.command()
@click.argument("file_id_or_name")
@click.option("--password", "-p", help="Decryption password", envvar="TELEVAULT_PASSWORD")
def cat(file_id_or_name: str, password: str | None):
    """Stream file content to stdout.

    For text files: outputs content directly (pipeable).
    For images: displays inline in terminal (Kitty/iTerm2).
    For video/audio/binary: shows error message and metadata.

    """

    async def _cat():
        vault = TeleVault(password=password)
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        import os

        index = await vault.telegram.get_index()
        if file_id_or_name in index.files:
            metadata = await vault.telegram.get_metadata(index.files[file_id_or_name])
        else:
            files = await vault.list_files()
            matches = [f for f in files if f.name == file_id_or_name or file_id_or_name in f.name]
            if not matches:
                console.print(f"[red]Error: File not found: {file_id_or_name}[/red]")
                await vault.disconnect()
                sys.exit(1)
            if len(matches) > 1:
                console.print(f"[red]Error: Multiple files match '{file_id_or_name}'[/red]")
                await vault.disconnect()
                sys.exit(1)
            metadata = matches[0]

        from .preview import classify_file

        file_type = classify_file(metadata.name)

        if file_type == "video":
            console.print("[yellow]Cannot display video files in terminal.[/yellow]")
            console.print(f"  File: {metadata.name}")
            console.print(f"  Size: {format_size(metadata.size)}")
            console.print("Use 'tvt pull' to download the file.")
            await vault.disconnect()
            return

        if file_type == "audio":
            console.print("[yellow]Cannot play audio files in terminal.[/yellow]")
            console.print(f"  File: {metadata.name}")
            console.print(f"  Size: {format_size(metadata.size)}")
            console.print("Use 'tvt pull' to download the file.")
            await vault.disconnect()
            return

        if file_type == "image":
            is_kitty = bool(
                os.environ.get("TERM") == "xterm-kitty" or os.environ.get("KITTY_WINDOW_ID")
            )
            is_iterm = os.environ.get("TERM_PROGRAM") == "iTerm.app"

            if is_kitty or is_iterm:
                try:
                    import tempfile
                    from pathlib import Path

                    ext = Path(metadata.name).suffix.lower()
                    with tempfile.NamedTemporaryFile(suffix=ext, delete=False) as tmp:
                        tmp_path = Path(tmp.name)

                    await vault.download(
                        file_id_or_name, output_path=str(tmp_path), password=password
                    )

                    with open(tmp_path, "rb") as f:
                        img_data = f.read()

                    if is_kitty:
                        import base64

                        b64 = base64.b64encode(img_data).decode("ascii")
                        esc = f"\x1b_Ga=T,f=100,s={len(img_data)},{b64}\x1b\\"
                        sys.stdout.buffer.write(esc.encode("ascii"))
                        sys.stdout.buffer.write(b"\n")
                        sys.stdout.buffer.flush()
                    elif is_iterm:
                        import base64

                        b64 = base64.b64encode(img_data).decode("ascii")
                        esc = (
                            f"\x1b]1337;File=name={metadata.name};"
                            f"inline=1;size={len(img_data)}:{b64}\x07"
                        )
                        sys.stdout.write(esc)
                        sys.stdout.write("\n")
                        sys.stdout.flush()

                    tmp_path.unlink(missing_ok=True)
                    await vault.disconnect()
                    return
                except Exception:
                    pass

            console.print("[yellow]Terminal does not support inline image display.[/yellow]")
            console.print(f"  File: {metadata.name}")
            console.print(f"  Size: {format_size(metadata.size)}")
            console.print("Use 'tvt pull' to download, then open the file.")
            await vault.disconnect()
            return

        try:
            total = await vault.stream(
                file_id_or_name,
                output=sys.stdout.buffer,
                password=password,
            )
            if os.isatty(sys.stdout.fileno()):
                print(f"\n[{format_size(total)} streamed]", file=sys.stderr)
        except FileNotFoundError:
            print(f"Error: File not found: {file_id_or_name}", file=sys.stderr)
            await vault.disconnect()
            sys.exit(1)
        except (ValueError, RuntimeError) as e:
            print(f"Error: {e}", file=sys.stderr)
            await vault.disconnect()
            sys.exit(1)
        except BrokenPipeError:
            pass
        finally:
            await vault.disconnect()

    run_async(_cat())


@main.command()
@click.argument("file_id_or_name")
@click.option("--password", "-p", help="Decryption password", envvar="TELEVAULT_PASSWORD")
@click.option(
    "--size", type=click.Choice(["small", "medium", "large"]), default="small", help="Preview size"
)
def preview(file_id_or_name: str, password: str | None, size: str):
    """Show a preview of a file without downloading it entirely.

    Detects file type and shows appropriate preview:
    - Text files: first ~40 lines of content
    - Images: dimensions, format, and metadata
    - Video/Audio: format and metadata info
    - Other: hex dump of first 256 bytes

    """

    async def _preview():
        from .preview import PreviewEngine

        engine = PreviewEngine(password=password)
        try:
            await engine._ensure_connected()

            if not await engine._vault.is_authenticated():
                console.print("[red]Not logged in. Run 'tvt login' first.[/red]")
                return

            if not engine._vault.config.channel_id:
                console.print("[red]No channel configured. Run 'tvt setup' first.[/red]")
                return

            await engine._vault.telegram.set_channel(engine._vault.config.channel_id)

            result = await engine.preview(file_id_or_name, size=size, password=password)

            console.print(f"\n[bold]{result.name}[/bold] ({result.file_type})")
            console.print(f"  Size: {format_size(result.size)}")
            console.print(f"  ID:   {result.file_id}")

            if result.metadata:
                for key, value in result.metadata.items():
                    console.print(f"  {key}: {value}")

            if result.file_type == "video":
                console.print()
                console.print("[dim]Video preview is not supported in terminal.[/dim]")
                console.print(f"[dim]Use 'tvt pull {result.name}' to download the file.[/dim]")
            elif result.file_type == "audio":
                console.print()
                console.print("[dim]Audio preview is not supported in terminal.[/dim]")
                console.print(f"[dim]Use 'tvt pull {result.name}' to download the file.[/dim]")
            elif result.file_type == "image":
                console.print()
                console.print(
                    f"[dim]Use 'tvt cat {result.name}' to display this image inline.[/dim]"
                )
                console.print(f"[dim]Use 'tvt pull {result.name}' to download the file.[/dim]")

            if result.preview_text and result.file_type not in ("video", "audio"):
                console.print()
                if result.file_type == "text":
                    console.print("[dim]--- Preview ---[/dim]")
                    for line in result.preview_text.splitlines()[:40]:
                        console.print(line)
                    total_lines = len(result.preview_text.splitlines())
                    if total_lines > 40:
                        console.print(f"[dim]... {total_lines - 40} more lines[/dim]")
                    console.print("[dim]--- End preview ---[/dim]")
                    console.print(f"[dim]Use 'tvt cat {result.name}' to see the full file.[/dim]")
                else:
                    console.print(result.preview_text)

        except FileNotFoundError as e:
            console.print(f"[red]Error: {e}[/red]")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
        finally:
            await engine.disconnect()

    run_async(_preview())


@main.command()
@click.option("--json", "as_json", is_flag=True, help="Output as JSON")
def stat(as_json: bool):
    """Show vault statistics."""

    async def _stat():
        vault = TeleVault()
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        try:
            status = await vault.get_status()

            if as_json:
                import json

                click.echo(json.dumps(status, indent=2))
            else:
                console.print("[bold]TeleVault Status[/bold]\n")
                console.print(f"  Channel: {status['channel_id']}")
                console.print(f"  Files: {status['file_count']}")
                console.print(f"  Total size: {format_size(status['total_size'])}")
                console.print(f"  Stored size: {format_size(status['stored_size'])}")
                console.print(f"  Compression ratio: {status['compression_ratio']:.1%}")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
            console.print("\n[dim]Have you run 'tvt login' and 'tvt setup'?[/dim]")

        await vault.disconnect()

    run_async(_stat())


# find is an alias for search - Click doesn't support @main.command(alias='find'),
# so we reference the same command function
find = search


# === Backup Commands ===


@main.group()
def backup():
    """Manage backup snapshots."""
    pass


@backup.command(name="create")
@click.argument("path", type=click.Path(exists=True))
@click.option("--name", "-n", help="Snapshot name (auto-generated if not provided)")
@click.option("--password", "-p", help="Encryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--incremental", "-i", is_flag=True, help="Create incremental backup")
@click.option("--parent", help="Parent snapshot ID for incremental backup")
@click.option("--dry-run", is_flag=True, help="Show what would be backed up without uploading")
def backup_create(
    path: str,
    name: str | None,
    password: str | None,
    incremental: bool,
    parent: str | None,
    dry_run: bool,
):
    """Create a backup snapshot of a directory."""

    async def _create():
        from .backup import BackupEngine

        engine = BackupEngine(password=password)
        await engine.connect()

        if not await check_auth(engine._vault):
            await engine.disconnect()
            return

        if not await check_channel(engine._vault):
            await engine.disconnect()
            return

        try:
            result = await engine.create_snapshot(
                path=path,
                name=name,
                incremental=incremental,
                parent_id=parent,
                dry_run=dry_run,
            )

            if dry_run:
                console.print("\n[bold]Dry Run Results:[/bold]")
                console.print(f"  Path: {result['path']}")
                console.print(f"  Total files: {result['total_files']}")
                console.print(f"  Total size: {format_size(result['total_size'])}")
                console.print(f"  Files to upload: {result['upload_files']}")
                if result["skipped"] > 0:
                    console.print(f"  Skipped (unchanged): {result['skipped']}")
            else:
                console.print(f"\n[bold green]✓ Snapshot created: {result.name}[/bold green]")
                console.print(f"  ID: {result.id}")
                console.print(f"  Files: {result.file_count}")
                console.print(f"  Size: {format_size(result.total_size)}")
                console.print(f"  Stored: {format_size(result.stored_size)}")
                if result.parent_id:
                    console.print(f"  Parent: {result.parent_id}")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
        finally:
            await engine.disconnect()

    run_async(_create())


@backup.command(name="restore")
@click.argument("snapshot_id")
@click.option("--output", "-o", type=click.Path(), help="Output directory")
@click.option("--password", "-p", help="Decryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--files", "-f", multiple=True, help="Specific files to restore")
def backup_restore(snapshot_id: str, output: str | None, password: str | None, files: tuple):
    """Restore files from a backup snapshot."""

    async def _restore():
        from .backup import BackupEngine

        engine = BackupEngine(password=password)
        await engine.connect()

        if not await check_auth(engine._vault):
            await engine.disconnect()
            return

        if not await check_channel(engine._vault):
            await engine.disconnect()
            return

        try:
            result_path = await engine.restore_snapshot(
                snapshot_id=snapshot_id,
                output_path=output or ".",
                password=password,
                files=list(files) if files else None,
            )
            console.print(f"\n[bold green]✓ Restored to: {result_path}[/bold green]")
        except FileNotFoundError as e:
            console.print(f"[red]Error: {e}[/red]")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
        finally:
            await engine.disconnect()

    run_async(_restore())


@backup.command(name="list")
def backup_list():
    """List all backup snapshots."""

    async def _list():
        from .backup import BackupEngine

        engine = BackupEngine()
        await engine.connect()

        if not await check_auth(engine._vault):
            await engine.disconnect()
            return

        try:
            snapshots = await engine.list_snapshots()

            if not snapshots:
                console.print("[yellow]No snapshots found[/yellow]")
                return

            table = Table(title="TeleVault Snapshots")
            table.add_column("ID", style="cyan")
            table.add_column("Name", style="bold")
            table.add_column("Created", style="green")
            table.add_column("Files", justify="right")
            table.add_column("Size", justify="right")
            table.add_column("Type")

            import datetime

            for s in snapshots:
                created = datetime.datetime.fromtimestamp(s.created_at).strftime("%Y-%m-%d %H:%M")
                snap_type = "Incremental" if s.is_incremental else "Full"
                table.add_row(
                    s.id[:8],
                    s.name,
                    created,
                    str(s.file_count),
                    format_size(s.total_size),
                    snap_type,
                )

            console.print(table)
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
        finally:
            await engine.disconnect()

    run_async(_list())


@backup.command(name="delete")
@click.argument("snapshot_id")
@click.option("--yes", "-y", is_flag=True, help="Skip confirmation")
def backup_delete(snapshot_id: str, yes: bool):
    """Delete a backup snapshot."""

    async def _delete():
        from .backup import BackupEngine

        if not yes and not click.confirm(f"Delete snapshot {snapshot_id}?"):
            return

        engine = BackupEngine()
        await engine.connect()

        if not await check_auth(engine._vault):
            await engine.disconnect()
            return

        try:
            deleted = await engine.delete_snapshot(snapshot_id)
            if deleted:
                console.print(f"[green]✓ Deleted snapshot {snapshot_id}[/green]")
            else:
                console.print(f"[yellow]Snapshot not found: {snapshot_id}[/yellow]")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
        finally:
            await engine.disconnect()

    run_async(_delete())


@backup.command(name="prune")
@click.option("--keep-daily", default=7, help="Keep last N daily snapshots")
@click.option("--keep-weekly", default=4, help="Keep last N weekly snapshots")
@click.option("--keep-monthly", default=6, help="Keep last N monthly snapshots")
@click.option("--dry-run", is_flag=True, help="Show what would be pruned without deleting")
def backup_prune(keep_daily: int, keep_weekly: int, keep_monthly: int, dry_run: bool):
    """Prune old backup snapshots based on retention policy."""

    async def _prune():
        from .backup import BackupEngine

        engine = BackupEngine()
        await engine.connect()

        if not await check_auth(engine._vault):
            await engine.disconnect()
            return

        try:
            policy = {
                "keep_daily": keep_daily,
                "keep_weekly": keep_weekly,
                "keep_monthly": keep_monthly,
            }

            if dry_run:
                snapshots = await engine.list_snapshots()
                console.print("\n[bold]Retention policy:[/bold]")
                console.print(f"  Keep daily: {keep_daily}")
                console.print(f"  Keep weekly: {keep_weekly}")
                console.print(f"  Keep monthly: {keep_monthly}")
                console.print(f"\n  Total snapshots: {len(snapshots)}")
                console.print("  (Dry run - no snapshots deleted)")
                return

            deleted = await engine.prune_snapshots(policy)
            if deleted:
                console.print(f"\n[green]✓ Pruned {len(deleted)} snapshots[/green]")
                for sid in deleted:
                    console.print(f"  Deleted: {sid}")
            else:
                console.print("\n[yellow]No snapshots pruned[/yellow]")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
        finally:
            await engine.disconnect()

    run_async(_prune())


@backup.command(name="verify")
@click.argument("snapshot_id")
@click.option("--password", "-p", help="Decryption password", envvar="TELEVAULT_PASSWORD")
def backup_verify(snapshot_id: str, password: str | None):
    """Verify a backup snapshot's integrity."""

    async def _verify():
        from .backup import BackupEngine

        engine = BackupEngine(password=password)
        await engine.connect()

        if not await check_auth(engine._vault):
            await engine.disconnect()
            return

        try:
            result = await engine.verify_snapshot(snapshot_id)

            console.print(f"\n[bold]Verifying snapshot: {result.get('name', snapshot_id)}[/bold]")
            console.print(f"  Files: {result.get('total_files', 0)}")
            console.print(f"  Verified: {result.get('verified', 0)}")

            if result.get("valid"):
                console.print("[bold green]✓ All files verified[/bold green]")
            else:
                console.print("[bold red]✗ Verification failed[/bold red]")
                for error in result.get("errors", []):
                    console.print(f"  [red]- {error}[/red]")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
        finally:
            await engine.disconnect()

    run_async(_verify())


@main.command()
@click.option("--mount-point", "-m", required=True, type=click.Path(), help="Mount point directory")
@click.option("--password", "-p", help="Encryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--read-only", is_flag=True, help="Mount as read-only")
@click.option("--cache-dir", type=click.Path(), help="Local cache directory")
@click.option("--cache-size", default=100, type=int, help="LRU cache size in MB (default: 100)")
@click.option("--allow-other", is_flag=True, help="Allow other users to access the mount")
@click.option(
    "--foreground/--background", default=True, help="Run in foreground (default) or background"
)
def mount(
    mount_point: str,
    password: str | None,
    read_only: bool,
    cache_dir: str | None,
    cache_size: int,
    allow_other: bool,
    foreground: bool,
):
    """Mount TeleVault as a local filesystem (requires FUSE).

    Uses on-demand chunk fetching: only the chunks needed for each read
    are downloaded, not the entire file. Recently accessed chunks are
    kept in an LRU memory cache for fast re-reads.

    """

    try:
        from .fuse import mount_vault
    except ImportError:
        console.print("[red]Error: fusepy is required for FUSE mount.[/red]")
        console.print("Install with: pip install televault[fuse]")
        console.print("\nOn Linux, you may also need: sudo apt install fuse libfuse2")
        console.print("On macOS, install macFUSE from: https://macfuse.io/")
        sys.exit(1)

    mount_path = Path(mount_point)
    if not mount_path.exists():
        console.print(f"[red]Mount point does not exist: {mount_point}[/red]")
        console.print("[dim]Create it with: mkdir -p " + str(mount_path) + "[/dim]")
        sys.exit(1)

    if not mount_path.is_dir():
        console.print(f"[red]Mount point is not a directory: {mount_point}[/red]")
        sys.exit(1)

    console.print(f"[bold blue]Mounting TeleVault at {mount_point}[/bold blue]")
    if read_only:
        console.print("[dim]Mode: read-only[/dim]")
    console.print(f"[dim]  Cache: {cache_size}MB LRU, on-demand chunk fetching[/dim]")

    try:
        mount_vault(
            mount_point=mount_point,
            password=password,
            read_only=read_only,
            cache_dir=cache_dir,
            cache_size_mb=cache_size,
            foreground=foreground,
            allow_other=allow_other,
        )
    except Exception as e:
        console.print(f"[red]Mount error: {e}[/red]")
        sys.exit(1)


@main.command()
@click.option("--host", "-h", default="0.0.0.0", help="Host to bind to")
@click.option("--port", "-p", default=8080, type=int, help="Port to listen on")
@click.option("--password", "-P", help="Encryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--read-only", is_flag=True, help="Serve as read-only")
@click.option("--cache-dir", type=click.Path(), help="Local cache directory")
def serve(host: str, port: int, password: str | None, read_only: bool, cache_dir: str | None):
    """Start a WebDAV server to access the vault over HTTP."""

    try:
        import aiohttp
    except ImportError:
        console.print("[red]Error: aiohttp is required for the WebDAV server.[/red]")
        console.print("Install with: pip install televault[webdav]")
        sys.exit(1)

    async def _serve():
        from .webdav import run_webdav_server

        console.print(f"[bold blue]Starting WebDAV server on http://{host}:{port}/[/bold blue]")
        if read_only:
            console.print("[dim]Mode: read-only[/dim]")

        try:
            await run_webdav_server(
                host=host,
                port=port,
                password=password,
                read_only=read_only,
                cache_dir=cache_dir,
            )
        except KeyboardInterrupt:
            console.print("\n[yellow]Server stopped[/yellow]")
        except RuntimeError as e:
            console.print(f"[red]Error: {e}[/red]")
            sys.exit(1)

    run_async(_serve())


# === Auto-Backup Schedule Commands ===


@main.group(name="schedule")
def schedule_group():
    """Manage automatic backup schedules."""
    pass


@schedule_group.command(name="create")
@click.argument("path", type=click.Path(exists=True))
@click.option("--name", "-n", help="Schedule name (defaults to directory name)")
@click.option(
    "--interval",
    "-i",
    type=click.Choice(["hourly", "daily", "weekly", "monthly"]),
    default="daily",
    help="Backup interval",
)
@click.option("--password", "-p", help="Encryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--incremental", is_flag=True, help="Create incremental backups")
def schedule_create(
    path: str, name: str | None, interval: str, password: str | None, incremental: bool
):
    """Create a backup schedule for a directory."""

    from .schedule import create_schedule, generate_cron_entry

    if name is None:
        name = Path(path).name

    entry = create_schedule(
        name=name,
        path=path,
        interval=interval,
        password=password,
        incremental=incremental,
    )

    console.print(f"[bold green]Schedule created: {name}[/bold green]")
    console.print(f"  Path: {path}")
    console.print(f"  Interval: {interval}")
    console.print(f"  Incremental: {incremental}")
    console.print()
    console.print("[bold]To set up automatic execution, choose one:[/bold]")
    console.print()
    console.print("[bold]Option 1 - systemd timer (Linux):[/bold]")
    console.print(f"  televault schedule install {name}")
    console.print()
    console.print("[bold]Option 2 - cron (any Unix):[/bold]")
    cron = generate_cron_entry(name, entry)
    console.print(f"  Add to crontab: {cron}")
    console.println()
    console.print("[bold]Option 3 - Run manually:[/bold]")
    console.print(f"  televault schedule run {name}")


@schedule_group.command(name="list")
def schedule_list():
    """List all backup schedules."""
    from .schedule import list_schedules

    schedules = list_schedules()
    if not schedules:
        console.print("[yellow]No schedules configured[/yellow]")
        return

    table = Table(title="TeleVault Schedules")
    table.add_column("Name", style="bold")
    table.add_column("Path")
    table.add_column("Interval")
    table.add_column("Enabled")
    table.add_column("Incremental")
    table.add_column("Last Run")

    import datetime

    for s in schedules:
        last_run = "Never"
        if s.last_run:
            last_run = datetime.datetime.fromtimestamp(s.last_run).strftime("%Y-%m-%d %H:%M")
        table.add_row(
            s.name,
            s.path,
            s.interval,
            "[green]Yes[/green]" if s.enabled else "[red]No[/red]",
            "Yes" if s.incremental else "No",
            last_run,
        )

    console.print(table)


@schedule_group.command(name="run")
@click.argument("name")
def schedule_run(name: str):
    """Run a scheduled backup immediately."""
    from .schedule import run_schedule

    console.print(f"[bold]Running schedule: {name}[/bold]")
    result = run_schedule(name)

    if result.success:
        console.print("[bold green]Backup successful![/bold green]")
        console.print(f"  Snapshot ID: {result.snapshot_id}")
        console.print(f"  Files: {result.file_count}")
        console.print(f"  Size: {format_size(result.total_size)}")
    else:
        console.print(f"[bold red]Backup failed: {result.error}[/bold red]")


@schedule_group.command(name="delete")
@click.argument("name")
@click.option("--yes", "-y", is_flag=True, help="Skip confirmation")
def schedule_delete(name: str, yes: bool):
    """Delete a backup schedule."""
    from .schedule import delete_schedule

    if not yes and not click.confirm(f"Delete schedule '{name}'?"):
        return

    if delete_schedule(name):
        console.print(f"[green]Schedule deleted: {name}[/green]")
    else:
        console.print(f"[yellow]Schedule not found: {name}[/yellow]")


@schedule_group.command(name="install")
@click.argument("name")
def schedule_install(name: str):
    """Install a schedule as a systemd timer (Linux only)."""
    from .schedule import install_systemd_timer, list_schedules

    schedules = {s.name: s for s in list_schedules()}
    if name not in schedules:
        console.print(f"[red]Schedule not found: {name}[/red]")
        return

    entry = schedules[name]
    success = install_systemd_timer(name, entry)

    if success:
        console.print(f"[bold green]systemd timer installed: televault-{name}.timer[/bold green]")
        console.print("\nManage with:")
        console.print(f"  systemctl --user status televault-{name}.timer")
        console.print(f"  systemctl --user stop televault-{name}.timer")
        console.print(f"  journalctl --user -u televault-{name}.service")
    else:
        console.print("[red]Failed to install systemd timer[/red]")
        console.print("[dim]Make sure you're on Linux with systemd available[/dim]")


@schedule_group.command(name="uninstall")
@click.argument("name")
def schedule_uninstall(name: str):
    """Uninstall a systemd timer."""
    from .schedule import uninstall_systemd_timer

    success = uninstall_systemd_timer(name)
    if success:
        console.print(f"[green]Timer uninstalled: televault-{name}.timer[/green]")
    else:
        console.print("[yellow]Timer not found or already removed[/yellow]")


@schedule_group.command(name="show-systemd")
@click.argument("name")
def schedule_show_systemd(name: str):
    """Show the systemd unit files for a schedule (without installing)."""
    from .schedule import generate_systemd_unit, list_schedules

    schedules = {s.name: s for s in list_schedules()}
    if name not in schedules:
        console.print(f"[red]Schedule not found: {name}[/red]")
        return

    entry = schedules[name]
    unit_content = generate_systemd_unit(name, entry)
    console.print(unit_content)


@main.command()
@click.option("--path", "-p", multiple=True, help="Directory to watch (can specify multiple)")
@click.option("--password", "-P", help="Encryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--interval", default=5.0, type=float, help="Poll interval in seconds")
@click.option("--exclude", "-e", multiple=True, help="Patterns to exclude")
def watch(path: tuple[str, ...], password: str | None, interval: float, exclude: tuple[str, ...]):
    """Watch directories for changes and auto-upload."""

    from .watcher import FileWatcher

    async def _watch():
        watcher = FileWatcher(
            password=password,
            exclude_patterns=list(exclude) if exclude else None,
        )

        for p in path:
            try:
                watcher.add_watch(p)
            except ValueError as e:
                console.print(f"[red]Error: {e}[/red]")
                return

        if not path:
            console.print("[red]Error: Specify at least one directory with --path[/red]")
            console.print("[dim]Example: televault watch --path /data/documents[/dim]")
            return

        console.print(f"[bold blue]Watching {len(path)} director(ies) for changes[/bold blue]")
        console.print(f"  Poll interval: {interval}s")
        console.print("  Press Ctrl+C to stop\n")

        try:
            await watcher.watch(poll_interval=interval)
        except KeyboardInterrupt:
            console.print("\n[yellow]Watch stopped[/yellow]")
        finally:
            watcher.save_state()

    run_async(_watch())


if __name__ == "__main__":
    main()
