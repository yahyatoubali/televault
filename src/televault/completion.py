"""Shell completion support for TeleVault CLI."""

import json
from pathlib import Path

from .config import get_config_dir

SHELL_BASH = "bash"
SHELL_ZSH = "zsh"
SHELL_FISH = "fish"
SHELL_POWERSHELL = "powershell"


def get_completion_script(shell: str, prog_name: str = "tvt") -> str:
    """Generate completion script for the given shell."""

    if shell == SHELL_BASH:
        return _bash_completion(prog_name)
    elif shell == SHELL_ZSH:
        return _zsh_completion(prog_name)
    elif shell == SHELL_FISH:
        return _fish_completion(prog_name)
    elif shell == SHELL_POWERSHELL:
        return _powershell_completion(prog_name)
    else:
        raise ValueError(f"Unsupported shell: {shell}. Supported: bash, zsh, fish, powershell")


def _bash_completion(prog_name: str) -> str:
    return f"""# Bash completion for {prog_name}
_{prog_name}_completions() {{
    local cur prev commands opts
    cur="${{COMP_WORDS[COMP_CWORD]}}"
    prev="${{COMP_WORDS[COMP_CWORD-1]}}"

    commands="push pull ls rm cat preview info stat find verify gc login setup logout whoami mount serve watch tui backup schedule channel completion"

    # Subcommands for 'backup'
    local backup_cmds="create restore list delete prune verify"

    # Subcommands for 'schedule'
    local schedule_cmds="create list run delete install uninstall show-systemd"

    # Global options
    opts="-v --verbose --debug -h --help --json"

    # Command-specific options
    case "${{COMP_WORDS[1]}}" in
        push)
            opts="-p --password --no-compress --no-encrypt -r --recursive --resume --name"
            ;;
        pull)
            opts="-o --output -p --password --resume"
            ;;
        cat)
            opts="-p --password"
            ;;
        preview)
            opts="-p --password --size"
            ;;
        mount)
            opts="-m --mount-point -p --password --read-only --cache-dir --allow-other --foreground --background"
            ;;
        serve)
            opts="-h --host -p --port -P --password --read-only --cache-dir"
            ;;
        ls)
            opts="--json --sort"
            ;;
    esac

    # Handle subcommands
    case "${{COMP_WORDS[1]}}" in
        backup)
            if [ "$COMP_CWORD" -eq 2 ]; then
                COMPREPLY=($(compgen -W "$backup_cmds" -- "$cur"))
            else
                COMPREPLY=($(compgen -W "$opts" -- "$cur"))
            fi
            return 0
            ;;
        schedule)
            if [ "$COMP_CWORD" -eq 2 ]; then
                COMPREPLY=($(compgen -W "$schedule_cmds" -- "$cur"))
            else
                COMPREPLY=($(compgen -W "$opts" -- "$cur"))
            fi
            return 0
            ;;
    esac

    if [ "$COMP_CWORD" -eq 1 ]; then
        COMPREPLY=($(compgen -W "$commands" -- "$cur"))
    elif [ "$COMP_CWORD" -ge 2 ]; then
        # Try file completion for commands that take files
        case "${{COMP_WORDS[1]}}" in
            push|rm|verify|info|cat|preview)
                COMPREPLY=($(compgen -f -- "$cur"))
                ;;
            *)
                COMPREPLY=($(compgen -W "$opts" -- "$cur"))
                ;;
        esac
    fi

    return 0
}}

complete -F _{prog_name}_completions {prog_name}
"""


def _zsh_completion(prog_name: str) -> str:
    return f"""#compdef {prog_name}

_{prog_name}() {{
    local -a commands
    commands=(
        'push:Upload file or directory'
        'pull:Download a file'
        'ls:List all files'
        'rm:Delete a file'
        'cat:Stream file to stdout'
        'preview:Show file preview'
        'info:Show detailed file info'
        'stat:Show vault statistics'
        'find:Search files by name'
        'verify:Verify file integrity'
        'gc:Garbage collect orphaned messages'
        'login:Authenticate with Telegram'
        'setup:Set up storage channel'
        'logout:Clear session'
        'whoami:Show current account'
        'mount:Mount vault as filesystem'
        'serve:Start WebDAV server'
        'watch:Watch directories for changes'
        'tui:Launch interactive TUI'
        'backup:Manage backup snapshots'
        'schedule:Manage backup schedules'
        'completion:Generate shell completion'
    )

    _arguments -C \\
        '1:command:->command' \\
        '*::arg:->args'

    case $state in
        command)
            _describe 'command' commands
            ;;
        args)
            case $words[1] in
                push)
                    _files
                    ;;
                pull|rm|verify|info|cat|preview)
                    ;;
                backup)
                    local -a backup_cmds
                    backup_cmds=('create:Create backup' 'restore:Restore backup' 'list:List backups' 'delete:Delete backup' 'prune:Prune old backups' 'verify:Verify backup')
                    _describe 'backup command' backup_cmds
                    ;;
                schedule)
                    local -a schedule_cmds
                    schedule_cmds=('create:Create schedule' 'list:List schedules' 'run:Run schedule' 'delete:Delete schedule' 'install:Install systemd timer' 'uninstall:Uninstall timer' 'show-systemd:Show unit files')
                    _describe 'schedule command' schedule_cmds
                    ;;
                completion)
                    local -a shells
                    shells=('bash' 'zsh' 'fish' 'powershell')
                    _describe 'shell' shells
                    ;;
            esac
            ;;
    esac
}}

_{prog_name}
"""


def _fish_completion(prog_name: str) -> str:
    return f"""# Fish completion for {prog_name}

set -l commands push pull ls rm cat preview info stat find verify gc login setup logout whoami mount serve watch tui backup schedule channel completion

complete -c {prog_name} -f -n "not __fish_seen_subcommand_from $commands" -a "$commands"

complete -c {prog_name} -f -n "__fish_seen_subcommand_from push" -r -a "(__fish_complete_path)"
complete -c {prog_name} -f -n "__fish_seen_subcommand_from pull" -a ""
complete -c {prog_name} -f -n "__fish_seen_subcommand_from rm" -a ""
complete -c {prog_name} -f -n "__fish_seen_subcommand_from cat" -a ""
complete -c {prog_name} -f -n "__fish_seen_subcommand_from preview" -a ""

set -l backup_cmds create restore list delete prune verify
complete -c {prog_name} -f -n "__fish_seen_subcommand_from backup; and not __fish_seen_subcommand_from $backup_cmds" -a "$backup_cmds"

set -l schedule_cmds create list run delete install uninstall show-systemd
complete -c {prog_name} -f -n "__fish_seen_subcommand_from schedule; and not __fish_seen_subcommand_from $schedule_cmds" -a "$schedule_cmds"
"""


def _powershell_completion(prog_name: str) -> str:
    return f"""# PowerShell completion for {prog_name}
Register-ArgumentCompleter -CommandName {prog_name} -ScriptBlock {{
    param($commandName, $wordToComplete, $cursorPosition, $commandAst)

    $commands = @(
        'push', 'pull', 'ls', 'rm', 'cat', 'preview', 'info', 'stat',
        'find', 'verify', 'gc', 'login', 'setup', 'logout', 'whoami',
        'mount', 'serve', 'watch', 'tui', 'backup', 'schedule', 'completion'
    )

    $backupSubcmds = @('create', 'restore', 'list', 'delete', 'prune', 'verify')
    $scheduleSubcmds = @('create', 'list', 'run', 'delete', 'install', 'uninstall', 'show-systemd')

    if ($commandAst.CommandElements.Count -le 2) {{
        $commands | Where-Object {{ $_ -like "$wordToComplete*" }} | ForEach-Object {{
            [System.Management.Automation.CompletionResult]::new($_, $_, 'ParameterValue', $_)
        }}
    }}
}}
"""


def get_cache_file() -> Path:
    """Get the file ID/name cache file path."""
    cache_dir = get_config_dir() / "cache"
    cache_dir.mkdir(parents=True, exist_ok=True)
    return cache_dir / "file_ids.json"


def load_file_cache() -> dict:
    """Load cached file IDs and names."""
    cache_file = get_cache_file()
    if cache_file.exists():
        try:
            return json.loads(cache_file.read_text())
        except Exception:
            return {}
    return {}


def save_file_cache(files: list[dict]) -> None:
    """Save file IDs and names to cache for completion.

    Merges with existing cache rather than replacing it.
    """
    cache_file = get_cache_file()
    existing = load_file_cache()
    for f in files:
        existing[f.get("id", "")] = f.get("name", "")
    tmp_path = cache_file.with_suffix(".tmp")
    tmp_path.write_text(json.dumps(existing, indent=2))
    tmp_path.replace(cache_file)


def get_cached_file_ids() -> list[str]:
    """Get list of cached file IDs for completion."""
    cache = load_file_cache()
    return list(cache.keys())


def get_cached_file_names() -> list[str]:
    """Get list of cached file names for completion."""
    cache = load_file_cache()
    return list(cache.values())


def install_completion(shell: str, prog_name: str = "tvt") -> str:
    """Return instructions for installing completion for the given shell."""
    script = get_completion_script(shell, prog_name)

    if shell == SHELL_BASH:
        return (
            f"# Add this to your ~/.bashrc:\n"
            f'eval "$({prog_name} completion bash)"\n\n'
            f"# Or save the script and source it:\n"
            f"{prog_name} completion bash > ~/.config/{prog_name}/completion.bash\n"
            f'echo "source ~/.config/{prog_name}/completion.bash" >> ~/.bashrc\n\n'
            f"--- Completion Script ---\n\n{script}"
        )
    elif shell == SHELL_ZSH:
        fpath_dir = "~/.zfunc"
        return (
            f"# Add this to your ~/.zshrc:\n"
            f"fpath+=({fpath_dir})\n"
            f"autoload -U compinit && compinit\n\n"
            f"# Or save the script:\n"
            f"mkdir -p {fpath_dir}\n"
            f"{prog_name} completion zsh > {fpath_dir}/_{prog_name}\n"
            f"rm -f ~/.zcompdump  # rebuild cache\n\n"
            f"--- Completion Script ---\n\n{script}"
        )
    elif shell == SHELL_FISH:
        return (
            f"# Save to ~/.config/fish/completions/{prog_name}.fish:\n"
            f"{prog_name} completion fish > ~/.config/fish/completions/{prog_name}.fish\n\n"
            f"--- Completion Script ---\n\n{script}"
        )
    elif shell == SHELL_POWERSHELL:
        return (
            f"# Add to your PowerShell profile:\n"
            f"{prog_name} completion powershell | Out-String | Add-Content $PROFILE\n\n"
            f"--- Completion Script ---\n\n{script}"
        )
    return script
