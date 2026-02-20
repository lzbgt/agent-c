#!/usr/bin/env python3
import argparse
import json
from pathlib import Path


DEFAULT_TARGETS = ["http://127.0.0.1:8123", "http://127.0.0.1:8124"]


def parse_targets(raw: str | None) -> list[str]:
    if not raw:
        return []
    return [t.strip() for t in raw.split(",") if t.strip()]


def load_targets_from_state(state_path: Path) -> list[str]:
    if not state_path.exists():
        return []
    try:
        state = json.loads(state_path.read_text())
    except Exception:
        return []
    agents = state.get("agents") or []
    targets = [a.get("agentd_base") for a in agents if a.get("agentd_base")]
    if targets:
        return targets
    base = state.get("agentd_base")
    return [base] if base else []


def build_workflow(
    targets: list[str],
    goal: str,
    timeout_ms: int,
    poll_ms: int,
    bearer_env: str,
) -> dict:
    target_entries = [{"id": f"a{idx + 1}", "base_url": base} for idx, base in enumerate(targets)]
    return {
        "inputs": {"goal": goal},
        "tasks": [
            {
                "task_id": "COLLAB",
                "kind": "agentd_parallel",
                "agentd_parallel": {
                    "targets": target_entries,
                    "agentd_call": {
                        "op": "workflow_submit_and_wait",
                        "timeout_ms": timeout_ms,
                        "poll_ms": poll_ms,
                        "include_results": True,
                        "include_tasks": False,
                        "bearer_env": bearer_env,
                        "workflow": {
                            "tasks": [
                                {
                                    "task_id": "RUN",
                                    "request": {
                                        "prompt": "Remote agent: propose an approach for the goal: ${input.goal}",
                                        "no_session": True,
                                    },
                                }
                            ]
                        },
                    },
                    "aggregate": {
                        "mode": "first_ok",
                        "ok_pointer": "/ok",
                        "value_pointer": "/agentd/final/result/results_by_task/RUN/assistant_text",
                    },
                },
            }
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate an agentd_parallel demo workflow JSON.")
    parser.add_argument(
        "--state",
        default="out/devstack_state.json",
        help="Path to a devstack_state.json for target discovery.",
    )
    parser.add_argument(
        "--targets",
        default="",
        help="Comma-separated agentd base URLs. Overrides --state targets.",
    )
    parser.add_argument(
        "--output",
        default="out/workflows/agentd_parallel_demo.json",
        help="Output JSON path.",
    )
    parser.add_argument(
        "--goal",
        default="Draft a collaborative plan for a multi-agent workflow graph demo.",
        help="Goal injected as workflow input.",
    )
    parser.add_argument("--timeout-ms", type=int, default=120000, help="agentd_call timeout in ms.")
    parser.add_argument("--poll-ms", type=int, default=200, help="agentd_call poll interval in ms.")
    parser.add_argument(
        "--bearer-env",
        default="AGENTD_CALL_BEARER",
        help="Env var name for bearer auth.",
    )
    parser.add_argument("--print", action="store_true", help="Print JSON to stdout.")
    args = parser.parse_args()

    targets = parse_targets(args.targets)
    if not targets:
        targets = load_targets_from_state(Path(args.state))
    if not targets:
        targets = DEFAULT_TARGETS

    workflow = build_workflow(
        targets=targets,
        goal=args.goal,
        timeout_ms=args.timeout_ms,
        poll_ms=args.poll_ms,
        bearer_env=args.bearer_env,
    )
    payload = json.dumps(workflow, indent=2) + "\n"

    if args.print:
        print(payload)

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
