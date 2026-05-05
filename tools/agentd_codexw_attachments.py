#!/usr/bin/env python3
"""Attachment bridging for codexw broker workflow submissions."""

from __future__ import annotations

import base64
import mimetypes
import re
import urllib.parse
import urllib.request
from typing import Any, Callable


MAX_REMOTE_ATTACHMENT_BYTES = 32 * 1024 * 1024


def _clean_name(raw: Any, fallback: str = "attachment.bin") -> str:
    text = str(raw or "").strip()
    if not text:
        text = fallback
    name = re.sub(r"[^A-Za-z0-9._-]+", "_", text)
    name = name.strip("._-")
    return name or fallback


def _content_type(attachment: dict[str, Any], name: str) -> str:
    for key in ("content_type", "mime", "mime_type"):
        value = attachment.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    guessed = mimetypes.guess_type(name)[0]
    return guessed or "application/octet-stream"


def _kind(content_type: str, requested: Any) -> str:
    if isinstance(requested, str) and requested.strip():
        value = requested.strip().lower()
        if value in {"image", "file", "text"}:
            return value
    if content_type.lower().startswith("image/"):
        return "image"
    if content_type.lower().startswith("text/"):
        return "text"
    return "file"


def _name_from_url(url: str) -> str:
    parsed = urllib.parse.urlparse(url)
    return _clean_name((parsed.path.rsplit("/", 1)[-1] or "").strip(), "attachment.bin")


def _download_base64(url: str, timeout: float) -> str:
    request = urllib.request.Request(url, headers={"Accept": "*/*"}, method="GET")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=timeout) as response:
        data = response.read(MAX_REMOTE_ATTACHMENT_BYTES + 1)
    if len(data) > MAX_REMOTE_ATTACHMENT_BYTES:
        raise ValueError("remote attachment exceeds max attachment size")
    if not data:
        raise ValueError("remote attachment is empty")
    return base64.b64encode(data).decode("ascii")


def _upload_file_payload(attachment: dict[str, Any], timeout: float) -> dict[str, Any]:
    url = str(attachment.get("download_url") or attachment.get("url") or "").strip()
    name = _clean_name(attachment.get("filename") or attachment.get("name") or _name_from_url(url))
    content_type = _content_type(attachment, name)
    data_base64 = str(attachment.get("data_base64") or "").strip()
    if not data_base64:
        if not url:
            raise ValueError(f"attachment {name} is missing data_base64 or download_url")
        data_base64 = _download_base64(url, timeout)
    return {
        "name": name,
        "mime": content_type,
        "data_base64": data_base64,
        "_kind": _kind(content_type, attachment.get("kind")),
    }


def attach_workflow_input_files(
    agentd_request: Callable[[str, str, Any | None], Any],
    workflow_request: dict[str, Any],
    *,
    default_session_id: str,
    timeout: float,
) -> dict[str, Any]:
    tasks = workflow_request.get("tasks")
    if not isinstance(tasks, list):
        return workflow_request
    for task in tasks:
        if not isinstance(task, dict):
            continue
        request = task.get("request")
        if not isinstance(request, dict):
            continue
        attachments = request.pop("attachments", None)
        if not isinstance(attachments, list) or not attachments:
            continue
        files = [_upload_file_payload(item, timeout) for item in attachments if isinstance(item, dict)]
        if not files:
            continue
        session_id = str(request.get("session_id") or default_session_id).strip()
        if not session_id:
            raise ValueError("workflow attachment submission requires a session_id")
        upload_request = {
            "session_id": session_id,
            "files": [{key: value for key, value in file.items() if key != "_kind"} for file in files],
        }
        upload_response = agentd_request("POST", "/api/v1/session/upload", upload_request)
        uploaded = upload_response.get("files") if isinstance(upload_response, dict) else None
        if not isinstance(uploaded, list) or len(uploaded) != len(files):
            raise ValueError(f"agentd accepted {len(uploaded or [])} of {len(files)} workflow attachments")
        input_files = request.get("input_files")
        if not isinstance(input_files, list):
            input_files = []
        for source, accepted in zip(files, uploaded):
            if not isinstance(accepted, dict) or not str(accepted.get("path") or "").strip():
                raise ValueError("agentd upload response omitted accepted file path")
            input_files.append(
                {
                    "path": str(accepted["path"]),
                    "name": str(accepted.get("name") or source["name"]),
                    "mime": str(accepted.get("mime") or source["mime"]),
                    "kind": str(accepted.get("kind") or source["_kind"]),
                }
            )
        request["session_id"] = session_id
        request["no_session"] = False
        request["input_files"] = input_files
        workflow_request["allow_sessions"] = True
        if not str(workflow_request.get("session_id") or "").strip():
            workflow_request["session_id"] = session_id
    return workflow_request
