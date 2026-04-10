#!/usr/bin/env python3
import argparse
import ctypes
import json
import os
import sys


GET_API_V1_SYMBOL = "agentd_voice_media_engine_get_api_v1"
GET_API_V2_SYMBOL = "agentd_voice_media_engine_get_api_v2"


class ProviderV1(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("media_engine_kind", ctypes.c_char_p),
        ("native_media_supported", ctypes.c_int),
        ("create", ctypes.c_void_p),
        ("destroy", ctypes.c_void_p),
        ("initialize", ctypes.c_void_p),
        ("handle_remote_description", ctypes.c_void_p),
        ("handle_remote_candidate", ctypes.c_void_p),
        ("handle_remote_bye", ctypes.c_void_p),
        ("handle_local_shutdown", ctypes.c_void_p),
    ]


class ProviderV2(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("media_engine_kind", ctypes.c_char_p),
        ("native_media_supported", ctypes.c_int),
        ("provider_name", ctypes.c_char_p),
        ("provider_version", ctypes.c_char_p),
        ("provider_capabilities_json", ctypes.c_char_p),
        ("create", ctypes.c_void_p),
        ("destroy", ctypes.c_void_p),
        ("initialize", ctypes.c_void_p),
        ("handle_remote_description", ctypes.c_void_p),
        ("handle_remote_candidate", ctypes.c_void_p),
        ("handle_remote_bye", ctypes.c_void_p),
        ("handle_local_shutdown", ctypes.c_void_p),
    ]


def text(value) -> str:
    if not value:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", "replace")
    return str(value)


def callback_presence(api) -> dict:
    return {
        "create": bool(api.create),
        "destroy": bool(api.destroy),
        "initialize": bool(api.initialize),
        "handle_remote_description": bool(api.handle_remote_description),
        "handle_remote_candidate": bool(api.handle_remote_candidate),
        "handle_remote_bye": bool(api.handle_remote_bye),
        "handle_local_shutdown": bool(api.handle_local_shutdown),
    }


def parse_capabilities(raw: str):
    raw = raw.strip()
    if not raw:
        return None
    caps = json.loads(raw)
    if not isinstance(caps, dict):
        raise ValueError("provider_capabilities_json must decode to an object")
    return caps


def inspect_provider(path: str) -> dict:
    out = {
        "ok": False,
        "library_path": os.path.abspath(path),
        "library_name": os.path.basename(path),
    }
    try:
        lib = ctypes.CDLL(path)
    except OSError as exc:
        out["error"] = f"dlopen failed: {exc}"
        return out

    get_v2 = getattr(lib, GET_API_V2_SYMBOL, None)
    if get_v2 is not None:
        get_v2.restype = ctypes.POINTER(ProviderV2)
        ptr = get_v2()
        if not ptr:
            out["error"] = "provider returned null v2 API"
            return out
        api = ptr.contents
        caps = None
        raw_caps = text(api.provider_capabilities_json)
        try:
            caps = parse_capabilities(raw_caps)
        except ValueError as exc:
            out["error"] = str(exc)
            return out
        callbacks = callback_presence(api)
        out.update(
            {
                "ok": True,
                "symbol": GET_API_V2_SYMBOL,
                "abi_version": int(api.abi_version),
                "media_engine_kind": text(api.media_engine_kind),
                "native_media_supported": bool(api.native_media_supported),
                "provider": {
                    "abi_version": int(api.abi_version),
                    "name": text(api.provider_name),
                    "version": text(api.provider_version),
                    "library_path": os.path.abspath(path),
                },
                "callbacks": callbacks,
            }
        )
        if caps is not None:
            out["provider"]["capabilities"] = caps
        if not all(callbacks.values()):
            out["ok"] = False
            out["error"] = "provider missing required callbacks"
        if not out["provider"]["name"]:
            out["ok"] = False
            out["error"] = "provider missing provider_name"
        return out

    get_v1 = getattr(lib, GET_API_V1_SYMBOL, None)
    if get_v1 is None:
        out["error"] = f"missing {GET_API_V2_SYMBOL} and {GET_API_V1_SYMBOL}"
        return out
    get_v1.restype = ctypes.POINTER(ProviderV1)
    ptr = get_v1()
    if not ptr:
        out["error"] = "provider returned null v1 API"
        return out
    api = ptr.contents
    callbacks = callback_presence(api)
    out.update(
        {
            "ok": True,
            "symbol": GET_API_V1_SYMBOL,
            "abi_version": int(api.abi_version),
            "media_engine_kind": text(api.media_engine_kind),
            "native_media_supported": bool(api.native_media_supported),
            "provider": {
                "abi_version": int(api.abi_version),
                "name": os.path.basename(path),
                "version": "legacy_abi_v1",
                "library_path": os.path.abspath(path),
                "capabilities": {"legacy_abi_v1": True},
            },
            "callbacks": callbacks,
        }
    )
    if not all(callbacks.values()):
        out["ok"] = False
        out["error"] = "provider missing required callbacks"
    return out


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Inspect an agentd builtin voice media-engine provider shared library."
    )
    parser.add_argument("library", help="Path to the provider shared library (.so/.dylib/.dll)")
    parser.add_argument("--pretty", action="store_true", help="Pretty-print JSON output")
    args = parser.parse_args()

    result = inspect_provider(args.library)
    json.dump(result, sys.stdout, indent=2 if args.pretty else None, sort_keys=True)
    sys.stdout.write("\n")
    return 0 if result.get("ok") else 1


if __name__ == "__main__":
    raise SystemExit(main())
