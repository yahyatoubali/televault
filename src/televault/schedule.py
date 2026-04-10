"""Automatic backup scheduler for TeleVault - systemd timers and cron integration."""

import json
import logging
import os
import platform
import subprocess
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path

from .config import get_config_dir, get_data_dir

logger = logging.getLogger("televault.schedule")


@dataclass
class ScheduleConfig:
    """Configuration for a scheduled backup."""

    name: str = "default"
    path: str = ""
    interval: str = "daily"
    retention_daily: int = 7
    retention_weekly: int = 4
    retention_monthly: int = 6
    password: str | None = None
    enabled: bool = True
    incremental: bool = False

    @classmethod
    def from_file(cls, path: Path) -> "ScheduleConfig":
        if not path.exists():
            return cls()
        with open(path) as f:
            data = json.load(f)
        known = {k for k in cls.__dataclass_fields__}
        return cls(**{k: v for k, v in data.items() if k in known})

    def to_file(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w") as f:
            json.dump(asdict(self), f, indent=2)


@dataclass
class ScheduleEntry:
    """A single scheduled backup job."""

    name: str
    path: str
    interval: str
    enabled: bool = True
    password: str | None = None
    last_run: float | None = None
    last_status: str | None = None
    incremental: bool = False

    def to_dict(self) -> dict:
        d = asdict(self)
        if self.password:
            d["password"] = "***"
        return d


@dataclass
class ScheduleResult:
    """Result of a scheduled backup run."""

    name: str
    success: bool
    snapshot_id: str | None = None
    file_count: int = 0
    total_size: int = 0
    error: str | None = None
    timestamp: float = field(default_factory=time.time)


def get_schedule_dir() -> Path:
    return get_config_dir() / "schedules"


def get_schedule_config(name: str) -> Path:
    return get_schedule_dir() / f"{name}.json"


def list_schedules() -> list[ScheduleEntry]:
    schedule_dir = get_schedule_dir()
    schedule_dir.mkdir(parents=True, exist_ok=True)

    entries = []
    for config_file in sorted(schedule_dir.glob("*.json")):
        try:
            data = json.loads(config_file.read_text())
            entry = ScheduleEntry(
                name=data.get("name", config_file.stem),
                path=data.get("path", ""),
                interval=data.get("interval", "daily"),
                enabled=data.get("enabled", True),
                password=data.get("password"),
                last_run=data.get("last_run"),
                last_status=data.get("last_status"),
                incremental=data.get("incremental", False),
            )
            entries.append(entry)
        except Exception as e:
            logger.warning(f"Failed to load schedule {config_file.name}: {e}")

    return entries


def create_schedule(
    name: str,
    path: str,
    interval: str = "daily",
    password: str | None = None,
    incremental: bool = False,
) -> ScheduleEntry:
    config_path = get_schedule_config(name)
    config_path.parent.mkdir(parents=True, exist_ok=True)

    entry = ScheduleEntry(
        name=name,
        path=path,
        interval=interval,
        password=password,
        incremental=incremental,
    )

    data = {
        "name": name,
        "path": path,
        "interval": interval,
        "enabled": True,
        "password": password,
        "incremental": incremental,
    }
    config_path.write_text(json.dumps(data, indent=2))
    logger.info(f"Created schedule: {name} ({interval}, path={path})")
    return entry


def delete_schedule(name: str) -> bool:
    config_path = get_schedule_config(name)
    if config_path.exists():
        config_path.unlink()
        logger.info(f"Deleted schedule: {name}")
        return True
    return False


def run_schedule(name: str) -> ScheduleResult:
    """Run a scheduled backup immediately."""
    import asyncio

    from .backup import BackupEngine
    from .snapshot import RetentionPolicy

    config_path = get_schedule_config(name)
    if not config_path.exists():
        return ScheduleResult(name=name, success=False, error=f"Schedule '{name}' not found")

    data = json.loads(config_path.read_text())
    path = data.get("path", "")
    password = data.get("password")
    incremental = data.get("incremental", False)

    if not path or not Path(path).exists():
        return ScheduleResult(name=name, success=False, error=f"Path does not exist: {path}")

    async def _run():
        engine = BackupEngine(password=password)
        try:
            await engine.connect()
            snapshot = await engine.create_snapshot(
                path=path,
                name=f"{name}-{time.strftime('%Y%m%d-%H%M%S')}",
                incremental=incremental,
            )
            return ScheduleResult(
                name=name,
                success=True,
                snapshot_id=snapshot.id,
                file_count=snapshot.file_count,
                total_size=snapshot.total_size,
            )
        except Exception as e:
            logger.error(f"Schedule '{name}' failed: {e}")
            return ScheduleResult(name=name, success=False, error=str(e))
        finally:
            await engine.disconnect()

    result = asyncio.run(_run())

    data["last_run"] = time.time()
    data["last_status"] = "success" if result.success else "failed"
    config_path.write_text(json.dumps(data, indent=2))

    return result


def generate_systemd_unit(name: str, schedule: ScheduleEntry) -> str:
    """Generate a systemd timer + service unit for a schedule."""
    interval_map = {
        "hourly": "1h",
        "daily": "1d",
        "weekly": "1w",
        "monthly": "1m",
    }
    on_calendar = {
        "hourly": "Hourly",
        "daily": "Daily",
        "weekly": "Weekly",
        "monthly": "Monthly",
    }

    timer = f"""[Unit]
Description=TeleVault backup: {name}
After=network-online.target
Wants=network-online.target

[Timer]
OnCalendar={on_calendar.get(schedule.interval, "Daily")}
Persistent=true
RandomizedDelaySec=300

[Install]
WantedBy=timers.target
"""

    password_arg = ""
    if schedule.password:
        password_arg = f' --password "{schedule.password}"'

    incremental_arg = " --incremental" if schedule.incremental else ""

    service = f"""[Unit]
Description=TeleVault backup: {name}
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=televault backup create "{schedule.path}" --name "{name}"{password_arg}{incremental_arg}
"""

    return f"# televault-{name}.timer\n{timer}\n# televault-{name}.service\n{service}"


def install_systemd_timer(name: str, schedule: ScheduleEntry) -> bool:
    """Generate and optionally install a systemd timer for a schedule."""
    if platform.system() != "Linux":
        logger.error("systemd timers are only available on Linux")
        return False

    timer_dir = Path.home() / ".config" / "systemd" / "user"
    timer_dir.mkdir(parents=True, exist_ok=True)

    interval_map = {
        "hourly": "Hourly",
        "daily": "Daily",
        "weekly": "Weekly",
        "monthly": "Monthly",
    }

    on_calendar = interval_map.get(schedule.interval, "Daily")

    password_arg = ""
    if schedule.password:
        password_arg = f' --password "{schedule.password}"'
    incremental_arg = " --incremental" if schedule.incremental else ""

    timer_content = f"""[Unit]
Description=TeleVault backup: {name}
After=network-online.target
Wants=network-online.target

[Timer]
OnCalendar={on_calendar}
Persistent=true
RandomizedDelaySec=300

[Install]
WantedBy=timers.target
"""

    service_content = f"""[Unit]
Description=TeleVault backup: {name}
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=televault backup create "{schedule.path}" --name "{name}"{password_arg}{incremental_arg}
"""

    timer_path = timer_dir / f"televault-{name}.timer"
    service_path = timer_dir / f"televault-{name}.service"

    timer_path.write_text(timer_content)
    service_path.write_text(service_content)

    try:
        subprocess.run(["systemctl", "--user", "daemon-reload"], check=True, capture_output=True)
        subprocess.run(
            ["systemctl", "--user", "enable", f"televault-{name}.timer"],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["systemctl", "--user", "start", f"televault-{name}.timer"],
            check=True,
            capture_output=True,
        )
        logger.info(f"Installed systemd timer: televault-{name}.timer")
        return True
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        logger.error(f"Failed to install systemd timer: {e}")
        return False


def uninstall_systemd_timer(name: str) -> bool:
    """Remove a systemd timer for a schedule."""
    if platform.system() != "Linux":
        return False

    try:
        subprocess.run(
            ["systemctl", "--user", "stop", f"televault-{name}.timer"], capture_output=True
        )
        subprocess.run(
            ["systemctl", "--user", "disable", f"televault-{name}.timer"], capture_output=True
        )
    except FileNotFoundError:
        pass

    timer_dir = Path.home() / ".config" / "systemd" / "user"
    timer_path = timer_dir / f"televault-{name}.timer"
    service_path = timer_dir / f"televault-{name}.service"

    removed = False
    if timer_path.exists():
        timer_path.unlink()
        removed = True
    if service_path.exists():
        service_path.unlink()
        removed = True

    if removed:
        try:
            subprocess.run(["systemctl", "--user", "daemon-reload"], capture_output=True)
        except FileNotFoundError:
            pass

    return removed


def generate_cron_entry(name: str, schedule: ScheduleEntry) -> str:
    """Generate a crontab entry for a schedule."""
    cron_map = {
        "hourly": "0 * * * *",
        "daily": "0 2 * * *",
        "weekly": "0 2 * * 0",
        "monthly": "0 2 1 * *",
    }

    cron_expr = cron_map.get(schedule.interval, "0 2 * * *")
    password_arg = ""
    if schedule.password:
        password_arg = f' --password "{schedule.password}"'
    incremental_arg = " --incremental" if schedule.incremental else ""

    return f'{cron_expr} televault backup create "{schedule.path}" --name "{name}"{password_arg}{incremental_arg}'
