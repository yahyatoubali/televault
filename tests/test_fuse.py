"""Tests for TeleVault FUSE filesystem (unit tests without FUSE mount)."""

import stat

import pytest

from televault.fuse import FUSE_AVAILABLE


class TestFuseAvailability:
    def test_fuse_module_imports(self):
        if FUSE_AVAILABLE:
            from televault.fuse import TeleVaultFuse

            assert TeleVaultFuse is not None
        else:
            with pytest.raises(ImportError):
                from fuse import FUSE

    def test_stat_helper(self):
        if not FUSE_AVAILABLE:
            pytest.skip("fusepy not available")

        from televault.fuse import TeleVaultFuse

        class FakeVault:
            pass

        fuse = TeleVaultFuse.__new__(TeleVaultFuse)
        fuse.read_only = False
        fuse._file_cache = {}
        fuse._path_to_id = {}
        fuse._id_to_path = {}

        dir_stat = fuse._get_stat(is_dir=True)
        assert stat.S_ISDIR(dir_stat["st_mode"])
        assert dir_stat["st_nlink"] == 2

        file_stat = fuse._get_stat(is_dir=False, size=1024)
        assert stat.S_ISREG(file_stat["st_mode"])
        assert file_stat["st_size"] == 1024

        fuse.read_only = True
        ro_stat = fuse._get_stat(is_dir=False, size=512)
        assert ro_stat["st_mode"] & 0o444
        assert not (ro_stat["st_mode"] & 0o222)
