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

console = Console()


def format_size(size: int) -> str:
    """Format bytes as human readable."""
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if size < 1024:
            return f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} PB"


def run_async(coro):
    """Run async function."""
    return asyncio.get_event_loop().run_until_complete(coro)


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
@click.pass_context
def main(ctx, help):
    """TeleVault - Unlimited cloud storage using Telegram."""
    if help or ctx.invoked_subcommand is None:
        click.echo(ctx.get_help())


@main.command()
@click.option("--phone", "-p", help="Phone number for login")
def login(phone: str | None):
    """Login to Telegram."""

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
def push(
    file_path: str, password: str | None, no_compress: bool, no_encrypt: bool, recursive: bool
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
def pull(file_id_or_name: str, output: str | None, password: str | None):
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


if __name__ == "__main__":
    main()
