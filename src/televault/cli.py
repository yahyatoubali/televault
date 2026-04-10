"""TeleVault CLI - Command line interface."""

import asyncio
import sys
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
    console.print("\n[bold]Method 3 - Use the TUI:[/bold]")
    console.print("  Run: televault tui")
    console.print("  The TUI will prompt you for credentials\n")

    console.print("[dim]For more information, see: https://my.telegram.org[/dim]")


def run_async(coro):
    """Run async function."""
    try:
        loop = asyncio.get_running_loop()
    except RuntimeError:
        loop = None

    if loop and loop.is_running():
        import concurrent.futures

        with concurrent.futures.ThreadPoolExecutor() as executor:
            future = executor.submit(asyncio.run, coro)
            return future.result()
    else:
        return asyncio.run(coro)


async def check_auth(vault: TeleVault) -> bool:
    """Check if user is authenticated. Returns True if authenticated, False otherwise."""
    if not await vault.is_authenticated():
        console.print("[red]Not logged in. Run 'televault login' first.[/red]")
        return False
    return True


async def check_channel(vault: TeleVault) -> bool:
    """Check if channel is set. Returns True if set, False otherwise."""
    if vault.config.channel_id is None:
        console.print("[red]No storage channel configured. Run 'televault setup' first.[/red]")
        return False
    return True


@click.group(invoke_without_command=True)
@click.option("-h", "--help", is_flag=True, help="Show this message and exit.")
@click.option("-v", "--verbose", is_flag=True, help="Enable verbose logging.")
@click.option("--debug", is_flag=True, help="Enable debug logging.")
@click.pass_context
def main(ctx, help, verbose, debug):
    """TeleVault - Unlimited cloud storage using Telegram."""
    if debug:
        setup_logging("DEBUG")
    elif verbose:
        setup_logging("INFO")
    else:
        setup_logging("WARNING")

    if help or ctx.invoked_subcommand is None:
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
@click.option("--channel-id", "-c", type=int, help="Existing channel ID to use")
@click.option("--auto-create", is_flag=True, help="Auto-create a new channel without prompting")
def setup(channel_id: int | None, auto_create: bool):
    """Set up storage channel."""

    async def _setup():
        vault = TeleVault()
        await vault.connect()

        # Check authentication first
        if not await check_auth(vault):
            await vault.disconnect()
            return

        # If channel_id is provided via CLI, use it
        if channel_id:
            cid = await vault.setup_channel(channel_id)
            console.print(f"[green]✓ Using existing channel: {cid}[/green]")
            console.print("[dim]Note: Make sure the bot is a member of this channel.[/dim]")
        elif auto_create:
            # Auto-create without prompting
            console.print("[bold]Creating new storage channel...[/bold]")
            cid = await vault.setup_channel()
            console.print(f"[green]✓ Created new channel: {cid}[/green]")
        else:
            # Interactive mode - ask user what they want to do
            console.print("[bold blue]TeleVault Storage Channel Setup[/bold blue]\n")
            console.print("How would you like to set up your storage?")
            console.print("  1. Create a new private channel (recommended)")
            console.print("  2. Use an existing channel by ID")
            console.print("")

            choice = input("Enter your choice (1 or 2): ").strip()

            if choice == "1":
                console.print("\n[bold]Creating new storage channel...[/bold]")
                cid = await vault.setup_channel()
                console.print(f"[green]✓ Created new channel: {cid}[/green]")
            elif choice == "2":
                console.print("\n[bold]Using existing channel[/bold]")
                console.print(
                    "[dim]Note: The channel ID should start with -100 (e.g., -1001234567890)[/dim]"
                )

                try:
                    existing_id = input("Enter channel ID: ").strip()
                    existing_id_int = int(existing_id)
                    cid = await vault.setup_channel(existing_id_int)
                    console.print(f"[green]✓ Using existing channel: {cid}[/green]")
                except ValueError:
                    console.print("[red]✗ Invalid channel ID. Please provide a valid number.[/red]")
                    await vault.disconnect()
                    return
                except Exception as e:
                    console.print(f"[red]✗ Error setting up channel: {e}[/red]")
                    await vault.disconnect()
                    return
            else:
                console.print("[red]✗ Invalid choice. Please enter 1 or 2.[/red]")
                await vault.disconnect()
                return

        await vault.disconnect()

    run_async(_setup())


@main.command()
@click.argument("file_path", type=click.Path(exists=True))
@click.option("--password", "-p", help="Encryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--no-compress", is_flag=True, help="Disable compression")
@click.option("--no-encrypt", is_flag=True, help="Disable encryption")
@click.option("--recursive", "-r", is_flag=True, help="Upload directory recursively")
@click.option("--resume", is_flag=True, help="Resume interrupted upload")
def push(
    file_path: str,
    password: str | None,
    no_compress: bool,
    no_encrypt: bool,
    recursive: bool,
    resume: bool,
):
    """Upload a file or directory to TeleVault."""

    async def _push():
        config = Config.load_or_create()

        if no_compress:
            config.compression = False
        if no_encrypt:
            config.encryption = False

        if config.encryption and not password:
            console.print("[yellow]Warning: Encryption enabled but no password provided.[/yellow]")
            console.print("Set password with --password or TELEVAULT_PASSWORD env var.")
            console.print("Use --no-encrypt to disable encryption.\n")

        vault = TeleVault(config=config, password=password)
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        file_path_obj = Path(file_path)

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
            file_size = file_path_obj.stat().st_size

            with Progress(
                SpinnerColumn(),
                TextColumn("[progress.description]{task.description}"),
                BarColumn(),
                TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
                TextColumn("({task.fields[size]})"),
                TimeRemainingColumn(),
                console=console,
                refresh_per_second=10,
            ) as progress:
                task = progress.add_task(
                    f"Uploading {file_path_obj.name}", total=100, size=format_size(file_size)
                )

                def on_progress(p: UploadProgress):
                    progress.update(task, completed=p.percent)

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
@click.option("--output", "-o", type=click.Path(), help="Output path")
@click.option("--password", "-p", help="Decryption password", envvar="TELEVAULT_PASSWORD")
@click.option("--resume", is_flag=True, help="Resume interrupted download")
def pull(file_id_or_name: str, output: str | None, password: str | None, resume: bool):
    """Download a file from TeleVault."""

    async def _pull():
        vault = TeleVault(password=password)
        await vault.connect()

        if not await check_auth(vault):
            await vault.disconnect()
            return

        if not await check_channel(vault):
            await vault.disconnect()
            return

        with Progress(
            SpinnerColumn(),
            TextColumn("[progress.description]{task.description}"),
            BarColumn(),
            TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
            TimeRemainingColumn(),
            console=console,
            refresh_per_second=10,
        ) as progress:
            task = progress.add_task(f"Downloading {file_id_or_name}", total=100)

            def on_progress(p: DownloadProgress):
                progress.update(task, completed=p.percent)

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

            output = [{"id": f.id, "name": f.name, "size": f.size} for f in files]
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
def search(query: str):
    """Search files by name."""

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

        if not files:
            console.print(f"[dim]No files matching '{query}'[/dim]")
        else:
            for f in files:
                console.print(f"[cyan]{f.id[:8]}[/cyan] {f.name} ({format_size(f.size)})")

        await vault.disconnect()

    run_async(_search())


@main.command()
@click.argument("file_id_or_name")
def info(file_id_or_name: str):
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

            console.print(f"[bold]{f.name}[/bold]\n")
            console.print(f"  ID:          {f.id}")
            console.print(f"  Size:        {format_size(f.size)}")
            console.print(f"  Hash:        {f.hash}")
            console.print(f"  Chunks:      {f.chunk_count}")
            console.print(f"  Encrypted:   {'Yes 🔒' if f.encrypted else 'No'}")
            console.print(f"  Compressed:  {'Yes' if f.compressed else 'No'}")
            if f.compressed and f.compression_ratio:
                console.print(f"  Comp. ratio: {f.compression_ratio:.1%}")
            if f.mime_type:
                console.print(f"  MIME type:   {f.mime_type}")

            from datetime import datetime

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
def status():
    """Show vault status."""

    async def _status():
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

            console.print("[bold]TeleVault Status[/bold]\n")
            console.print(f"  Channel: {status['channel_id']}")
            console.print(f"  Files: {status['file_count']}")
            console.print(f"  Total size: {format_size(status['total_size'])}")
            console.print(f"  Stored size: {format_size(status['stored_size'])}")
            console.print(f"  Compression ratio: {status['compression_ratio']:.1%}")
        except Exception as e:
            console.print(f"[red]Error: {e}[/red]")
            console.print("\n[dim]Have you run 'televault login' and 'televault setup'?[/dim]")

        await vault.disconnect()

    run_async(_status())


@main.command()
def whoami():
    """Show current Telegram account."""

    async def _whoami():
        vault = TeleVault()
        await vault.connect()

        if not await vault.telegram._client.is_user_authorized():
            console.print("[red]Not logged in. Run 'televault login' first.[/red]")
            await vault.disconnect()
            return

        me = await vault.telegram._client.get_me()

        if me is None:
            console.print("[red]Not logged in. Run 'televault login' first.[/red]")
            await vault.disconnect()
            return

        console.print(f"[bold]{me.first_name}[/bold]", end="")
        if me.last_name:
            console.print(f" {me.last_name}", end="")
        console.print()

        if me.username:
            console.print(f"  @{me.username}")
        console.print(f"  ID: {me.id}")

        await vault.disconnect()

    run_async(_whoami())


@main.command()
def tui():
    """Launch the interactive TUI."""
    from .tui import run_tui

    run_tui()


@main.command(name="gc")
@click.option("--dry-run", is_flag=True, help="Show orphans without deleting them")
@click.option("--clean-partials", is_flag=True, help="Also remove incomplete uploads")
def garbage_collect(dry_run: bool, clean_partials: bool):
    """Find and remove orphaned messages from the vault."""

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

        console.print("[bold]Scanning for orphaned messages...[/bold]")
        result = await collect_garbage(vault.telegram, dry_run=dry_run)

        if not result["orphaned_messages"]:
            console.print("[green]No orphaned messages found. Vault is clean![/green]")
        else:
            from .cli import format_size

            count = len(result["orphaned_messages"])
            size = result["orphaned_size"]
            console.print(f"  Found {count} orphaned messages ({format_size(size)})")

            if dry_run:
                console.print("[yellow]Dry run - no messages deleted.[/yellow]")
                for msg in result["orphaned_messages"][:10]:
                    console.print(
                        f"  - Message {msg['id']} ({msg['type']}, {format_size(msg['size'])})"
                    )
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
@click.option("--allow-other", is_flag=True, help="Allow other users to access the mount")
@click.option(
    "--foreground/--background", default=True, help="Run in foreground (default) or background"
)
def mount(
    mount_point: str,
    password: str | None,
    read_only: bool,
    cache_dir: str | None,
    allow_other: bool,
    foreground: bool,
):
    """Mount TeleVault as a local filesystem (requires FUSE)."""

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

    try:
        mount_vault(
            mount_point=mount_point,
            password=password,
            read_only=read_only,
            cache_dir=cache_dir,
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


if __name__ == "__main__":
    main()
