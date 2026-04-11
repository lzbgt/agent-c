#!/usr/bin/env python3
"""Reference agentd tool server for ESP32 serial/MQTT bridges.

The process speaks agentd's strict JSON-lines tool-server protocol on
stdin/stdout and maps advertised UM-ACDS device tools to UM-BMP TASK_ASSIGN
envelopes for an ESP32-class node.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import sys
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any


BRIDGE_SCHEMA = "agentd.esp32_bridge_tool_server.v1"


class BridgeError(RuntimeError):
    pass


def emit(obj: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(obj, separators=(",", ":"), sort_keys=True) + "\n")
    sys.stdout.flush()


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def utc_ms() -> int:
    return int(time.time() * 1000)


def compact_json(obj: Any) -> str:
    return json.dumps(obj, separators=(",", ":"), sort_keys=True)


def compute_caps_sha256(manifest: dict[str, Any]) -> str:
    normalized = copy.deepcopy(manifest)
    normalized.pop("caps_sha256", None)
    return hashlib.sha256(compact_json(normalized).encode("utf-8")).hexdigest()


def default_manifest(node_id: str) -> dict[str, Any]:
    manifest: dict[str, Any] = {
        "spec_version": "um-acds/0.1",
        "manifest_version": "tool-server-reference/0.1",
        "caps_sha256": "",
        "node": {
            "node_id": node_id,
            "model": "esp32-bridge-reference",
        },
        "runtime": {
            "bridge": {
                "schema": BRIDGE_SCHEMA,
                "agent_core": "external-node",
            }
        },
        "hardware": {
            "presence": {
                "system.status": "present",
                "ui.led.ws2812": "present",
                "gpio.output": "unknown",
            }
        },
        "tools": [
            {
                "name": "system.device_status",
                "description": "Read the ESP32 device status snapshot.",
                "kind": "system",
                "parameters_schema": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "include_detail": {"type": "boolean"},
                    },
                    "required": [],
                },
                "timeout_ms": 1000,
                "idempotent": True,
                "side_effect_level": "none",
                "hazards": [],
                "result_schema": {
                    "type": "object",
                    "additionalProperties": True,
                },
            },
            {
                "name": "ui.led.ws2812.control",
                "description": "Set a WS2812 LED color/effect on the ESP32 node.",
                "kind": "ui",
                "parameters_schema": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "mode": {"type": "string", "enum": ["off", "solid", "rainbow"]},
                        "rgb": {
                            "type": "array",
                            "items": {"type": "integer", "minimum": 0, "maximum": 255},
                            "minItems": 3,
                            "maxItems": 3,
                        },
                    },
                    "required": ["mode"],
                },
                "timeout_ms": 1000,
                "idempotent": False,
                "side_effect_level": "low",
                "hazards": ["visible_light"],
            },
            {
                "name": "gpio.output.set",
                "description": "Set a named ESP32 GPIO output through the bridge.",
                "kind": "actuator",
                "parameters_schema": {
                    "type": "object",
                    "additionalProperties": False,
                    "properties": {
                        "channel": {"type": "string"},
                        "value": {"type": "boolean"},
                    },
                    "required": ["channel", "value"],
                },
                "timeout_ms": 1000,
                "idempotent": False,
                "side_effect_level": "high",
                "hazards": ["gpio", "external_actuator"],
            },
        ],
        "safety": {
            "default_policy": "platform_gate_high_side_effects",
        },
        "tags": ["role:esp32-bridge", "transport:stdio-tool-server"],
        "transport_hints": {
            "serial": {"line_protocol": "jsonl", "baud_default": 115200},
            "mqtt": {
                "request_topic": "agentd/edge/{node_id}/in",
                "response_topic": "agentd/edge/{node_id}/out",
            },
        },
    }
    manifest["caps_sha256"] = "sha256:" + compute_caps_sha256(manifest)
    return manifest


def load_manifest(path: str | None, node_id: str) -> dict[str, Any]:
    if not path:
        return default_manifest(node_id)
    try:
        raw = json.loads(Path(path).read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        raise BridgeError(f"failed to read manifest {path}: {exc}") from exc
    if isinstance(raw, dict) and isinstance(raw.get("body"), dict) and isinstance(raw["body"].get("manifest"), dict):
        return raw["body"]["manifest"]
    if isinstance(raw, dict) and isinstance(raw.get("manifest"), dict):
        return raw["manifest"]
    if not isinstance(raw, dict):
        raise BridgeError("manifest must be a JSON object")
    return raw


def manifest_node_id(manifest: dict[str, Any], fallback: str) -> str:
    node = manifest.get("node")
    if isinstance(node, dict) and isinstance(node.get("node_id"), str) and node["node_id"].strip():
        return node["node_id"].strip()
    return fallback


def manifest_tools(manifest: dict[str, Any]) -> list[dict[str, Any]]:
    tools = manifest.get("tools")
    if not isinstance(tools, list):
        raise BridgeError("UM-ACDS manifest must contain tools[]")
    out: list[dict[str, Any]] = []
    for tool in tools:
        if not isinstance(tool, dict):
            continue
        name = str(tool.get("name") or "").strip()
        params = tool.get("parameters_schema")
        if not name or not isinstance(params, dict):
            continue
        out.append(tool)
    if not out:
        raise BridgeError("UM-ACDS manifest did not expose any tools with parameters_schema")
    return out


def agentd_tool_name(um_acds_name: str) -> str:
    slug = "".join(
        ch if ("a" <= ch <= "z") or ("A" <= ch <= "Z") or ("0" <= ch <= "9") or ch in "_-" else "_"
        for ch in um_acds_name.strip()
    ).strip("_")
    if not slug:
        slug = "tool"
    name = f"esp32_{slug}"
    if len(name) > 64:
        digest = hashlib.sha256(um_acds_name.encode("utf-8")).hexdigest()[:10]
        name = f"{name[:53]}_{digest}"
    return name


def build_agentd_tool_map(manifest: dict[str, Any]) -> dict[str, dict[str, Any]]:
    out: dict[str, dict[str, Any]] = {}
    for tool in manifest_tools(manifest):
        um_name = str(tool.get("name") or "").strip()
        safe_name = agentd_tool_name(um_name)
        if safe_name in out:
            digest = hashlib.sha256(um_name.encode("utf-8")).hexdigest()[:10]
            safe_name = f"{safe_name[:53]}_{digest}"
        if safe_name in out:
            raise BridgeError(f"UM-ACDS tool name collision after agentd-safe mapping: {um_name}")
        out[safe_name] = tool
    return out


def agentd_tool_manifest(manifest: dict[str, Any], tool_map: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    caps_sha = manifest.get("caps_sha256") if isinstance(manifest.get("caps_sha256"), str) else ""
    node_id = manifest_node_id(manifest, "")
    tools = []
    for safe_name, tool in tool_map.items():
        um_name = str(tool["name"])
        schema = copy.deepcopy(tool["parameters_schema"])
        if not isinstance(schema, dict):
            schema = {"type": "object", "additionalProperties": True}
        tools.append(
            {
                "name": safe_name,
                "description": str(tool.get("description") or f"Bridge UM-ACDS tool {um_name} to ESP32 node."),
                "parameters": schema,
                "bridge": {
                    "schema": BRIDGE_SCHEMA,
                    "node_id": node_id,
                    "caps_sha256": caps_sha,
                    "um_acds_name": um_name,
                    "um_acds_kind": tool.get("kind", ""),
                    "side_effect_level": tool.get("side_effect_level", ""),
                    "hazards": tool.get("hazards", []),
                },
            }
        )
    return tools


def validate_simple_schema(schema: dict[str, Any], args: dict[str, Any]) -> None:
    if schema.get("type") not in (None, "object"):
        raise BridgeError("tool parameters_schema root must be an object")
    required = schema.get("required")
    if isinstance(required, list):
        for key in required:
            if isinstance(key, str) and key not in args:
                raise BridgeError(f"missing required argument: {key}")
    props = schema.get("properties")
    props = props if isinstance(props, dict) else {}
    if schema.get("additionalProperties") is False:
        unknown = sorted(k for k in args if k not in props)
        if unknown:
            raise BridgeError(f"unknown argument(s): {', '.join(unknown)}")
    for key, spec in props.items():
        if key not in args or not isinstance(spec, dict):
            continue
        expected = spec.get("type")
        val = args[key]
        if expected == "string" and not isinstance(val, str):
            raise BridgeError(f"argument {key} must be string")
        if expected == "boolean" and not isinstance(val, bool):
            raise BridgeError(f"argument {key} must be boolean")
        if expected == "integer" and (not isinstance(val, int) or isinstance(val, bool)):
            raise BridgeError(f"argument {key} must be integer")
        if expected == "number" and (not isinstance(val, (int, float)) or isinstance(val, bool)):
            raise BridgeError(f"argument {key} must be number")
        if expected == "object" and not isinstance(val, dict):
            raise BridgeError(f"argument {key} must be object")
        if expected == "array" and not isinstance(val, list):
            raise BridgeError(f"argument {key} must be array")
        enum = spec.get("enum")
        if isinstance(enum, list) and val not in enum:
            raise BridgeError(f"argument {key} must be one of {enum}")


def build_task_assign(
    *,
    node_id: str,
    agentd_tool_name: str,
    um_acds_tool_name: str,
    arguments: dict[str, Any],
    timeout_ms: int,
    request_id: Any,
) -> dict[str, Any]:
    task_id = f"tool:{agentd_tool_name}:{uuid.uuid4().hex[:12]}"
    return {
        "msg_id": f"bridge:{uuid.uuid4()}",
        "ts_utc_ms": utc_ms(),
        "type": "TASK_ASSIGN",
        "from": "platform:agentd_tool_server",
        "to": f"node:{node_id}",
        "body": {
            "node_id": node_id,
            "task_id": task_id,
            "step_id": um_acds_tool_name,
            "idempotency_key": f"tool_server:{request_id}:{task_id}",
            "mode": "invoke",
            "tool": um_acds_tool_name,
            "deadline_utc_ms": utc_ms() + max(1, timeout_ms),
            "attempt": 1,
            "payload": {
                "tool": um_acds_tool_name,
                "agentd_tool": agentd_tool_name,
                "args": arguments,
            },
        },
    }


@dataclass
class BridgeConfig:
    transport: str
    serial_device: str
    serial_baud: int
    mqtt_host: str
    mqtt_port: int
    mqtt_request_topic: str
    mqtt_response_topic: str
    timeout_ms: int


class BridgeTransport:
    def send(self, envelope: dict[str, Any]) -> dict[str, Any]:
        raise NotImplementedError


class DryRunTransport(BridgeTransport):
    def send(self, envelope: dict[str, Any]) -> dict[str, Any]:
        return {
            "type": "TASK_DONE",
            "body": {
                "task_id": envelope["body"]["task_id"],
                "step_id": envelope["body"]["step_id"],
                "idempotency_key": envelope["body"]["idempotency_key"],
                "result": {
                    "ok": True,
                    "data": {
                        "transport": "dry-run",
                        "task_assign": envelope,
                    },
                },
            },
        }


class SerialTransport(BridgeTransport):
    def __init__(self, cfg: BridgeConfig) -> None:
        self.cfg = cfg

    def send(self, envelope: dict[str, Any]) -> dict[str, Any]:
        if not self.cfg.serial_device:
            raise BridgeError("--serial-device is required for serial transport")
        try:
            import serial  # type: ignore[import-not-found]
        except Exception as exc:  # noqa: BLE001
            raise BridgeError("serial transport requires pyserial") from exc

        deadline = time.monotonic() + (self.cfg.timeout_ms / 1000.0)
        payload = compact_json(envelope) + "\n"
        expected_key = envelope["body"]["idempotency_key"]
        with serial.Serial(self.cfg.serial_device, self.cfg.serial_baud, timeout=0.2) as port:
            port.write(payload.encode("utf-8"))
            port.flush()
            while time.monotonic() < deadline:
                raw = port.readline()
                if not raw:
                    continue
                try:
                    msg = json.loads(raw.decode("utf-8"))
                except Exception:
                    continue
                if response_matches(msg, expected_key):
                    return msg
        raise BridgeError("serial transport timed out waiting for matching response")


class MqttTransport(BridgeTransport):
    def __init__(self, cfg: BridgeConfig, node_id: str) -> None:
        self.cfg = cfg
        self.node_id = node_id

    def send(self, envelope: dict[str, Any]) -> dict[str, Any]:
        try:
            import paho.mqtt.client as mqtt  # type: ignore[import-not-found]
        except Exception as exc:  # noqa: BLE001
            raise BridgeError("mqtt transport requires paho-mqtt") from exc

        request_topic = self.cfg.mqtt_request_topic.format(node_id=self.node_id)
        response_topic = self.cfg.mqtt_response_topic.format(node_id=self.node_id)
        expected_key = envelope["body"]["idempotency_key"]
        messages: list[dict[str, Any]] = []

        def on_message(_client: Any, _userdata: Any, msg: Any) -> None:
            try:
                parsed = json.loads(msg.payload.decode("utf-8"))
            except Exception:
                return
            if response_matches(parsed, expected_key):
                messages.append(parsed)

        client = mqtt.Client()
        client.on_message = on_message
        client.connect(self.cfg.mqtt_host, self.cfg.mqtt_port, keepalive=30)
        client.subscribe(response_topic)
        client.loop_start()
        try:
            published = client.publish(request_topic, compact_json(envelope), qos=1)
            published.wait_for_publish(timeout=max(1.0, self.cfg.timeout_ms / 1000.0))
            deadline = time.monotonic() + (self.cfg.timeout_ms / 1000.0)
            while time.monotonic() < deadline:
                if messages:
                    return messages[0]
                time.sleep(0.05)
        finally:
            client.loop_stop()
            client.disconnect()
        raise BridgeError("mqtt transport timed out waiting for matching response")


def response_matches(msg: Any, expected_key: str) -> bool:
    if not isinstance(msg, dict):
        return False
    body = msg.get("body")
    if isinstance(body, dict) and body.get("idempotency_key") == expected_key:
        return True
    return msg.get("idempotency_key") == expected_key


def build_transport(cfg: BridgeConfig, node_id: str) -> BridgeTransport:
    if cfg.transport == "dry-run":
        return DryRunTransport()
    if cfg.transport == "serial":
        return SerialTransport(cfg)
    if cfg.transport == "mqtt":
        return MqttTransport(cfg, node_id)
    raise BridgeError(f"unsupported transport: {cfg.transport}")


def normalize_tool_result(resp: dict[str, Any]) -> dict[str, Any]:
    msg_type = resp.get("type")
    body = resp.get("body") if isinstance(resp.get("body"), dict) else {}
    if msg_type == "TASK_DONE":
        result = body.get("result")
        if isinstance(result, dict):
            return result
        return {"ok": True, "data": result}
    if msg_type == "TASK_FAILED":
        return {"ok": False, "error": str(body.get("error") or "task failed")}
    if "ok" in resp:
        return resp
    return {"ok": True, "data": resp}


def handle_execute(
    *,
    request_id: Any,
    tool_name: str,
    arguments: Any,
    by_name: dict[str, dict[str, Any]],
    node_id: str,
    transport: BridgeTransport,
    timeout_ms: int,
) -> None:
    tool = by_name.get(tool_name)
    if tool is None:
        emit({"id": request_id, "ok": False, "error": f"unknown tool: {tool_name}"})
        return
    if not isinstance(arguments, dict):
        emit({"id": request_id, "ok": False, "error": "arguments must be object"})
        return
    try:
        validate_simple_schema(tool["parameters_schema"], arguments)
        um_acds_tool_name = str(tool["name"])
        envelope = build_task_assign(
            node_id=node_id,
            agentd_tool_name=tool_name,
            um_acds_tool_name=um_acds_tool_name,
            arguments=arguments,
            timeout_ms=timeout_ms,
            request_id=request_id,
        )
        response = transport.send(envelope)
        tool_result = normalize_tool_result(response)
        emit({"id": request_id, "ok": bool(tool_result.get("ok", True)), "tool_result": tool_result})
    except BridgeError as exc:
        tool_result = {"ok": False, "error": str(exc)}
        emit({"id": request_id, "ok": False, "error": str(exc), "tool_result": tool_result})
    except Exception as exc:  # noqa: BLE001
        tool_result = {"ok": False, "error": f"bridge error: {exc}"}
        emit({"id": request_id, "ok": False, "error": tool_result["error"], "tool_result": tool_result})


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="agentd ESP32 serial/MQTT tool-server bridge")
    parser.add_argument("--manifest", default=os.environ.get("AGENTD_ESP32_BRIDGE_MANIFEST", ""))
    parser.add_argument("--node-id", default=os.environ.get("AGENTD_ESP32_BRIDGE_NODE_ID", "esp32-bridge-1"))
    parser.add_argument(
        "--transport",
        choices=["dry-run", "serial", "mqtt"],
        default=os.environ.get("AGENTD_ESP32_BRIDGE_TRANSPORT", "dry-run"),
    )
    parser.add_argument("--timeout-ms", type=int, default=int(os.environ.get("AGENTD_ESP32_BRIDGE_TIMEOUT_MS", "30000")))
    parser.add_argument("--serial-device", default=os.environ.get("AGENTD_ESP32_BRIDGE_SERIAL_DEVICE", ""))
    parser.add_argument("--serial-baud", type=int, default=int(os.environ.get("AGENTD_ESP32_BRIDGE_SERIAL_BAUD", "115200")))
    parser.add_argument("--mqtt-host", default=os.environ.get("AGENTD_ESP32_BRIDGE_MQTT_HOST", "127.0.0.1"))
    parser.add_argument("--mqtt-port", type=int, default=int(os.environ.get("AGENTD_ESP32_BRIDGE_MQTT_PORT", "1883")))
    parser.add_argument(
        "--mqtt-request-topic",
        default=os.environ.get("AGENTD_ESP32_BRIDGE_MQTT_REQUEST_TOPIC", "agentd/edge/{node_id}/in"),
    )
    parser.add_argument(
        "--mqtt-response-topic",
        default=os.environ.get("AGENTD_ESP32_BRIDGE_MQTT_RESPONSE_TOPIC", "agentd/edge/{node_id}/out"),
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    cfg = BridgeConfig(
        transport=args.transport,
        serial_device=args.serial_device,
        serial_baud=args.serial_baud,
        mqtt_host=args.mqtt_host,
        mqtt_port=args.mqtt_port,
        mqtt_request_topic=args.mqtt_request_topic,
        mqtt_response_topic=args.mqtt_response_topic,
        timeout_ms=max(1, args.timeout_ms),
    )
    try:
        manifest = load_manifest(args.manifest or None, args.node_id)
        node_id = manifest_node_id(manifest, args.node_id)
        by_name = build_agentd_tool_map(manifest)
        transport = build_transport(cfg, node_id)
    except BridgeError as exc:
        log(f"startup error: {exc}")
        return 2

    for line in sys.stdin:
        raw = line.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError as exc:
            emit({"id": None, "ok": False, "error": f"invalid json: {exc}"})
            continue
        if not isinstance(msg, dict):
            emit({"id": None, "ok": False, "error": "request must be object"})
            continue
        request_id = msg.get("id")
        op = msg.get("op")
        if op == "manifest":
            emit({"id": request_id, "ok": True, "tools": agentd_tool_manifest(manifest, by_name)})
            continue
        if op == "ping":
            emit({"id": request_id, "ok": True, "pong": True})
            continue
        if op == "execute":
            handle_execute(
                request_id=request_id,
                tool_name=str(msg.get("tool_name") or ""),
                arguments=msg.get("arguments"),
                by_name=by_name,
                node_id=node_id,
                transport=transport,
                timeout_ms=cfg.timeout_ms,
            )
            continue
        emit({"id": request_id, "ok": False, "error": f"unsupported op: {op}"})
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
