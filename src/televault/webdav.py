"""WebDAV server for TeleVault - access your vault over HTTP/WebDAV."""

import asyncio
import json
import logging
import os
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import unquote, urlparse

from .config import Config, get_data_dir
from .core import TeleVault
from .models import FileMetadata
from .telegram import TelegramConfig

logger = logging.getLogger("televault.webdav")

WEBDAV_NS = "DAV:"

RESPONSE_CONTENT_TYPES = {
    ".txt": "text/plain",
    ".html": "text/html",
    ".css": "text/css",
    ".js": "application/javascript",
    ".json": "application/json",
    ".xml": "application/xml",
    ".pdf": "application/pdf",
    ".png": "image/png",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".gif": "image/gif",
    ".svg": "image/svg+xml",
    ".mp3": "audio/mpeg",
    ".mp4": "video/mp4",
    ".zip": "application/zip",
    ".gz": "application/gzip",
    ".tar": "application/x-tar",
    ".doc": "application/msword",
    ".docx": "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    ".xls": "application/vnd.ms-excel",
    ".xlsx": "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
}


def make_multistatus_xml(responses: list[dict]) -> str:
    root = ET.Element("multistatus", xmlns=WEBDAV_NS)
    for resp in responses:
        response_el = ET.SubElement(root, "response")
        href_el = ET.SubElement(response_el, "href")
        href_el.text = resp["href"]
        propstat_el = ET.SubElement(response_el, "propstat")
        prop_el = ET.SubElement(propstat_el, "prop")
        for key, value in resp.get("props", {}).items():
            el = ET.SubElement(prop_el, key)
            el.text = str(value) if value is not None else ""
        status_el = ET.SubElement(propstat_el, "status")
        status_el.text = resp.get("status", "HTTP/1.1 200 OK")
    return ET.tostring(root, encoding="unicode", xml_declaration=True)


class WebDAVHandler:
    """WebDAV request handler using aiohttp-style interface."""

    def __init__(
        self,
        vault: TeleVault,
        cache_dir: Path | None = None,
        read_only: bool = False,
    ):
        self.vault = vault
        self.cache_dir = cache_dir or get_data_dir() / "webdav_cache"
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self.read_only = read_only
        self._file_cache: dict[str, FileMetadata] = {}
        self._last_refresh = 0.0

    async def _refresh_index(self):
        now = time.time()
        if now - self._last_refresh < 5.0:
            return
        files = await self.vault.list_files()
        self._file_cache.clear()
        for f in files:
            self._file_cache[f.name] = f
        self._last_refresh = now

    def _resolve_file(self, path: str) -> FileMetadata | None:
        name = path.strip("/").split("/")[-1] if path.strip("/") else ""
        return self._file_cache.get(name)

    async def handle_request(
        self, method: str, path: str, headers: dict, body: bytes = b""
    ) -> dict:
        await self._refresh_index()

        clean_path = unquote(path).rstrip("/")
        if clean_path == "":
            clean_path = "/"

        handler = {
            "GET": self._handle_get,
            "HEAD": self._handle_head,
            "PUT": self._handle_put,
            "DELETE": self._handle_delete,
            "PROPFIND": self._handle_propfind,
            "PROPPATCH": self._handle_proppatch,
            "MKCOL": self._handle_mkcol,
            "OPTIONS": self._handle_options,
            "LOCK": self._handle_lock,
        }.get(method)

        if handler is None:
            return {
                "status": 405,
                "headers": {"Allow": "GET, HEAD, PUT, DELETE, PROPFIND, OPTIONS"},
                "body": b"Method Not Allowed",
            }

        return await handler(clean_path, headers, body)

    async def _handle_options(self, path, headers, body):
        return {
            "status": 200,
            "headers": {
                "Allow": "GET, HEAD, PUT, DELETE, PROPFIND, PROPPATCH, MKCOL, OPTIONS, LOCK",
                "DAV": "1, 2",
                "Content-Length": "0",
            },
            "body": b"",
        }

    async def _handle_propfind(self, path, headers, body):
        depth = headers.get("Depth", "1")
        responses = []

        responses.append(
            {
                "href": "/",
                "props": {
                    "resourcetype": "collection",
                    "displayname": "TeleVault",
                },
                "status": "HTTP/1.1 200 OK",
            }
        )

        if depth in ("1", "infinity"):
            for name, meta in self._file_cache.items():
                responses.append(
                    {
                        "href": f"/{name}",
                        "props": {
                            "resourcetype": None,
                            "displayname": name,
                            "getcontentlength": str(meta.size),
                            "getlastmodified": time.strftime(
                                "%a, %d %b %Y %H:%M:%S GMT",
                                time.gmtime(meta.created_at),
                            ),
                            "getcontenttype": RESPONSE_CONTENT_TYPES.get(
                                Path(name).suffix.lower(), "application/octet-stream"
                            ),
                        },
                        "status": "HTTP/1.1 200 OK",
                    }
                )

        xml_body = make_multistatus_xml(responses)
        return {
            "status": 207,
            "headers": {"Content-Type": "application/xml; charset=utf-8"},
            "body": xml_body.encode("utf-8"),
        }

    async def _handle_get(self, path, headers, body):
        if path == "/":
            html = "<html><head><title>TeleVault</title></head><body>"
            html += "<h1>TeleVault WebDAV</h1><ul>"
            for name, meta in self._file_cache.items():
                size_str = _format_size(meta.size)
                html += f'<li><a href="/{name}">{name}</a> ({size_str})</li>'
            html += "</ul></body></html>"
            return {"status": 200, "headers": {"Content-Type": "text/html"}, "body": html.encode()}

        meta = self._resolve_file(path)
        if meta is None:
            return {"status": 404, "headers": {}, "body": b"Not Found"}

        local_path = self.cache_dir / meta.name
        if not local_path.exists():
            local_path.parent.mkdir(parents=True, exist_ok=True)
            await self.vault.download(meta.id, output_path=str(local_path))

        content_type = RESPONSE_CONTENT_TYPES.get(
            Path(meta.name).suffix.lower(), "application/octet-stream"
        )
        data = local_path.read_bytes()
        return {
            "status": 200,
            "headers": {
                "Content-Type": content_type,
                "Content-Length": str(len(data)),
                "Content-Disposition": f'attachment; filename="{meta.name}"',
            },
            "body": data,
        }

    async def _handle_head(self, path, headers, body):
        meta = self._resolve_file(path)
        if meta is None:
            return {"status": 404, "headers": {}, "body": b""}

        content_type = RESPONSE_CONTENT_TYPES.get(
            Path(meta.name).suffix.lower(), "application/octet-stream"
        )
        return {
            "status": 200,
            "headers": {
                "Content-Type": content_type,
                "Content-Length": str(meta.size),
            },
            "body": b"",
        }

    async def _handle_put(self, path, headers, body):
        if self.read_only:
            return {"status": 403, "headers": {}, "body": b"Forbidden"}

        filename = path.strip("/").split("/")[-1]
        if not filename:
            return {"status": 400, "headers": {}, "body": b"Bad Request"}

        local_path = self.cache_dir / filename
        local_path.parent.mkdir(parents=True, exist_ok=True)
        local_path.write_bytes(body)

        try:
            metadata = await self.vault.upload(local_path)
            self._file_cache[metadata.name] = metadata
            self._last_refresh = 0
            return {"status": 201, "headers": {}, "body": b"Created"}
        except Exception as e:
            logger.error(f"Upload failed: {e}")
            return {"status": 500, "headers": {}, "body": f"Upload failed: {e}".encode()}

    async def _handle_delete(self, path, headers, body):
        if self.read_only:
            return {"status": 403, "headers": {}, "body": b"Forbidden"}

        meta = self._resolve_file(path)
        if meta is None:
            return {"status": 404, "headers": {}, "body": b"Not Found"}

        try:
            await self.vault.delete(meta.id)
            self._file_cache.pop(meta.name, None)
            local_path = self.cache_dir / meta.name
            if local_path.exists():
                local_path.unlink()
            self._last_refresh = 0
            return {"status": 204, "headers": {}, "body": b""}
        except Exception as e:
            logger.error(f"Delete failed: {e}")
            return {"status": 500, "headers": {}, "body": f"Delete failed: {e}".encode()}

    async def _handle_proppatch(self, path, headers, body):
        return {"status": 200, "headers": {}, "body": b""}

    async def _handle_mkcol(self, path, headers, body):
        return {
            "status": 405,
            "headers": {"Allow": "GET, HEAD, PUT, DELETE, PROPFIND, OPTIONS"},
            "body": b"Not Allowed",
        }

    async def _handle_lock(self, path, headers, body):
        lock_token = "opaquelocktoken:televault-lock"
        xml = f'<?xml version="1.0" encoding="utf-8"?>\n<D:prop xmlns:D="DAV:">\n  <D:lockdiscovery>\n    <D:activelock>\n      <D:locktoken><D:href>{lock_token}</D:href></D:locktoken>\n    </D:activelock>\n  </D:lockdiscovery>\n</D:prop>'
        return {
            "status": 200,
            "headers": {"Content-Type": "application/xml", "Lock-Token": f"<{lock_token}>"},
            "body": xml.encode(),
        }


def _format_size(size: int) -> str:
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if size < 1024:
            return f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} PB"


class WebDAVServer:
    """Lightweight WebDAV server using aiohttp."""

    def __init__(
        self,
        config: Config | None = None,
        telegram_config: TelegramConfig | None = None,
        password: str | None = None,
        host: str = "0.0.0.0",
        port: int = 8080,
        read_only: bool = False,
        cache_dir: str | None = None,
    ):
        self.config = config or Config.load_or_create()
        self.password = password
        self.host = host
        self.port = port
        self.read_only = read_only
        self._telegram_config = telegram_config
        self._vault: TeleVault | None = None
        self._handler: WebDAVHandler | None = None
        self._cache_dir = Path(cache_dir) if cache_dir else get_data_dir() / "webdav_cache"
        self._server = None

    async def start(self):
        try:
            from aiohttp import web
        except ImportError:
            raise ImportError(
                "aiohttp is required for WebDAV server. Install with: pip install aiohttp"
            )

        self._vault = TeleVault(
            config=self.config,
            telegram_config=self._telegram_config,
            password=self.password,
        )
        await self._vault.connect()

        if not await self._vault.is_authenticated():
            raise RuntimeError("Not authenticated. Run 'televault login' first.")

        if not self._vault.config.channel_id:
            raise RuntimeError("No channel configured. Run 'televault setup' first.")

        await self._vault.telegram.set_channel(self._vault.config.channel_id)

        self._handler = WebDAVHandler(
            vault=self._vault,
            cache_dir=self._cache_dir,
            read_only=self.read_only,
        )

        app = web.Application()
        app.router.add_route("*", "/{path:.*}", self._handle_request)
        app.router.add_route("*", "/", self._handle_request)

        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, self.host, self.port)
        await site.start()
        self._server = runner

        logger.info(f"WebDAV server running on http://{self.host}:{self.port}/")

    async def _handle_request(self, request):
        if self._handler is None:
            return await self._error_response(503, "Server not ready")

        method = request.method
        path = urlparse(request.url.path).path
        headers = dict(request.headers)
        body = await request.read()

        try:
            result = await self._handler.handle_request(method, path, headers, body)
        except Exception as e:
            logger.error(f"Request error: {e}")
            return await self._error_response(500, str(e))

        response = web.Response(
            status=result["status"],
            body=result["body"],
        )
        for key, value in result.get("headers", {}).items():
            response.headers[key] = str(value)

        if method == "PROPFIND":
            response.headers["Content-Type"] = "application/xml; charset=utf-8"

        return response

    async def _error_response(self, status, message):
        from aiohttp import web

        return web.Response(status=status, text=message)

    async def stop(self):
        if self._server:
            await self._server.cleanup()
        if self._vault:
            await self._vault.disconnect()

    async def serve_forever(self):
        await self.start()
        try:
            while True:
                await asyncio.sleep(1)
        except asyncio.CancelledError:
            pass
        finally:
            await self.stop()


async def run_webdav_server(
    host: str = "0.0.0.0",
    port: int = 8080,
    config: Config | None = None,
    telegram_config: TelegramConfig | None = None,
    password: str | None = None,
    read_only: bool = False,
    cache_dir: str | None = None,
):
    server = WebDAVServer(
        config=config,
        telegram_config=telegram_config,
        password=password,
        host=host,
        port=port,
        read_only=read_only,
        cache_dir=cache_dir,
    )
    await server.serve_forever()
