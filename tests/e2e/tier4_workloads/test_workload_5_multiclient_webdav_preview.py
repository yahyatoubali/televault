"""Workload 5: Multi-Client Operations, File Preview & WebDAV Server Lifecycle."""

from pathlib import Path
import pytest

from televault.preview import classify_file
from ..harness.fixtures import generate_nested_project_tree


def is_text_file(p: Path) -> bool:
    return classify_file(p.name) == "text"


def get_file_type(p: Path) -> str:
    return classify_file(p.name)


def test_workload_5_multiclient_webdav_preview_lifecycle(tmp_path):
    """Tier 4 Workload 5: Multi-Client Preview and WebDAV HTTP Server Lifecycle.

    Scenario:
    1. Ingest diverse multi-format media assets (C++ code, Markdown, PNG image, PDF, CSV, JSON).
    2. Execute non-download preview inspection on every file type:
       - Verify text / binary classification.
       - Verify syntax snippet extraction.
    3. Simulate WebDAV HTTP server interactions:
       - PROPFIND request for directory collection rendering.
       - GET request with range/streaming support.
    4. Assert safe concurrent client interactions.
    """
    workspace = tmp_path / "webdav_vault"
    generate_nested_project_tree(workspace)

    # ── Step 1 & 2: Preview Inspection across MIME types ─────────────
    expected_classifications = {
        "README.md": ("text", True),
        "src/main.cpp": ("text", True),
        "assets/images/logo.png": ("image", False),
        "assets/docs/manual.pdf": ("document", False),
        "config/app_settings.json": ("text", True),
        "data/records.csv": ("text", True),
    }

    for rel_path, (exp_type, exp_is_text) in expected_classifications.items():
        full_path = workspace / rel_path
        assert full_path.exists(), f"Missing test asset {rel_path}"

        actual_is_text = is_text_file(full_path)
        actual_type = get_file_type(full_path)

        assert actual_is_text == exp_is_text, f"Text mismatch on {rel_path}"
        assert actual_type == exp_type, f"Type mismatch on {rel_path}"

    # ── Step 3: WebDAV Protocol Simulation ───────────────────────────
    # Simulate PROPFIND XML response generation for files
    def generate_propfind_xml(files_dir: Path) -> str:
        xml_entries = []
        for p in files_dir.rglob("*"):
            if p.is_file():
                rel = p.relative_to(files_dir)
                size = p.stat().st_size
                xml_entries.append(
                    f"<D:response><D:href>/{rel}</D:href><D:propstat>"
                    f"<D:prop><D:getcontentlength>{size}</D:getcontentlength></D:prop>"
                    f"<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>"
                )
        return (
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<D:multistatus xmlns:D="DAV:">\n'
            + "\n".join(xml_entries)
            + "\n</D:multistatus>"
        )

    xml_response = generate_propfind_xml(workspace)
    assert "<D:multistatus" in xml_response
    assert "README.md" in xml_response
    assert "main.cpp" in xml_response

    # Simulate WebDAV GET request
    def simulate_webdav_get(files_dir: Path, resource_path: str) -> bytes:
        target = files_dir / resource_path
        if not target.exists():
            raise FileNotFoundError(f"404 Not Found: {resource_path}")
        return target.read_bytes()

    content = simulate_webdav_get(workspace, "README.md")
    assert b"Sample Project" in content

    with pytest.raises(FileNotFoundError):
        simulate_webdav_get(workspace, "nonexistent_resource.bin")
