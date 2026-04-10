"""Tests for TeleVault WebDAV handler."""

import pytest

from televault.webdav import WebDAVHandler, _format_size, make_multistatus_xml


class TestFormatSize:
    def test_bytes(self):
        assert _format_size(100) == "100.0 B"

    def test_kilobytes(self):
        assert _format_size(1024) == "1.0 KB"

    def test_megabytes(self):
        assert _format_size(1024 * 1024) == "1.0 MB"

    def test_gigabytes(self):
        assert _format_size(1024 * 1024 * 1024) == "1.0 GB"

    def test_zero(self):
        assert _format_size(0) == "0.0 B"


class TestMultistatusXml:
    def test_single_response(self):
        responses = [
            {"href": "/", "props": {"resourcetype": "collection"}, "status": "HTTP/1.1 200 OK"}
        ]
        xml = make_multistatus_xml(responses)
        assert "href" in xml
        assert "/" in xml
        assert "collection" in xml

    def test_multiple_responses(self):
        responses = [
            {"href": "/", "props": {"resourcetype": "collection"}, "status": "HTTP/1.1 200 OK"},
            {
                "href": "/file.txt",
                "props": {"resourcetype": None, "getcontentlength": "42"},
                "status": "HTTP/1.1 200 OK",
            },
        ]
        xml = make_multistatus_xml(responses)
        assert "file.txt" in xml
        assert "42" in xml


class TestWebDAVHandlerOptions:
    @pytest.fixture
    def handler(self):
        class FakeVault:
            pass

        return WebDAVHandler(vault=FakeVault())

    @pytest.mark.asyncio
    async def test_options(self, handler):
        result = await handler._handle_options("/", {}, b"")
        assert result["status"] == 200
        assert "GET" in result["headers"]["Allow"]
        assert "DAV" in result["headers"]

    @pytest.mark.asyncio
    async def test_mkcol_not_allowed(self, handler):
        result = await handler._handle_mkcol("/newdir", {}, b"")
        assert result["status"] == 405

    @pytest.mark.asyncio
    async def test_proppatch(self, handler):
        result = await handler._handle_proppatch("/", {}, b"")
        assert result["status"] == 200

    @pytest.mark.asyncio
    async def test_lock(self, handler):
        result = await handler._handle_lock("/file.txt", {}, b"")
        assert result["status"] == 200
        assert "Lock-Token" in result["headers"]


class TestWebDAVHandlerReadOnly:
    @pytest.fixture
    def handler(self):
        class FakeVault:
            pass

        return WebDAVHandler(vault=FakeVault(), read_only=True)

    @pytest.mark.asyncio
    async def test_put_forbidden(self, handler):
        result = await handler._handle_put("/file.txt", {}, b"data")
        assert result["status"] == 403

    @pytest.mark.asyncio
    async def test_delete_forbidden(self, handler):
        result = await handler._handle_delete("/file.txt", {}, b"")
        assert result["status"] == 403
