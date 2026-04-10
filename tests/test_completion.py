"""Tests for TeleVault shell completion."""

import pytest

from televault.completion import (
    SHELL_BASH,
    SHELL_FISH,
    SHELL_POWERSHELL,
    SHELL_ZSH,
    get_completion_script,
    get_cached_file_ids,
    get_cached_file_names,
    load_file_cache,
    save_file_cache,
)


class TestCompletionScripts:
    def test_bash_completion(self):
        script = get_completion_script(SHELL_BASH, prog_name="tvt")
        assert "_tvt_completions" in script
        assert "complete -F" in script
        assert "push" in script
        assert "pull" in script
        assert "ls" in script
        assert "cat" in script
        assert "preview" in script
        assert "stat" in script
        assert "find" in script

    def test_zsh_completion(self):
        script = get_completion_script(SHELL_ZSH, prog_name="tvt")
        assert "#compdef tvt" in script
        assert "push" in script
        assert "backup" in script
        assert "schedule" in script

    def test_fish_completion(self):
        script = get_completion_script(SHELL_FISH, prog_name="tvt")
        assert "complete -c tvt" in script
        assert "push" in script

    def test_powershell_completion(self):
        script = get_completion_script(SHELL_POWERSHELL, prog_name="tvt")
        assert "Register-ArgumentCompleter" in script
        assert "push" in script

    def test_unsupported_shell(self):
        with pytest.raises(ValueError):
            get_completion_script("csh")


class TestFileCache:
    def test_save_and_load(self, tmp_path, monkeypatch):
        monkeypatch.setattr("televault.completion.get_config_dir", lambda: tmp_path / "config")
        (tmp_path / "config").mkdir(parents=True, exist_ok=True)

        files = [
            {"id": "abc123", "name": "photo.jpg", "size": 1024},
            {"id": "def456", "name": "doc.pdf", "size": 2048},
        ]
        save_file_cache(files)

        ids = get_cached_file_ids()
        assert "abc123" in ids
        assert "def456" in ids

        names = get_cached_file_names()
        assert "photo.jpg" in names
        assert "doc.pdf" in names

    def test_load_empty_cache(self, tmp_path, monkeypatch):
        monkeypatch.setattr(
            "televault.completion.get_config_dir", lambda: tmp_path / "empty_config"
        )
        (tmp_path / "empty_config").mkdir(parents=True, exist_ok=True)

        cache = load_file_cache()
        assert cache == {}
