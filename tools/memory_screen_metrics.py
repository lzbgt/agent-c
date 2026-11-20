#!/usr/bin/env python3
"""Offline evidence metrics for the frozen synthetic Agent-C screen.

This validates fixture bytes, namespace/revision/span references and reported
contexts. It is not an execution attestation or a semantic answer validator.
No provider request, credential lookup or production state access is performed.
"""
import argparse
import datetime as dt
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re


class InvalidScreen(ValueError):
    pass


def require(condition, reason):
    if not condition:
        raise InvalidScreen(reason)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    # JsonCpp uses 17 significant digits and preserves real-number ".0".
    # Python's shortest-roundtrip float encoder has different byte lengths.
    # Exact evidence identities themselves contain only strings and integers.
    def encode(v):
        if v is None:
            return "null"
        if type(v) is bool:
            return "true" if v else "false"
        if type(v) is int:
            return str(v)
        if type(v) is float:
            require(math.isfinite(v), "nonfinite JSON number")
            text = format(v, ".17g")
            return text if "." in text or "e" in text else text + ".0"
        if isinstance(v, str):
            return json.dumps(v, ensure_ascii=True)
        if isinstance(v, list):
            return "[" + ",".join(encode(x) for x in v) + "]"
        require(isinstance(v, dict), "unsupported JSON value")
        return "{" + ",".join(encode(k) + ":" + encode(v[k]) for k in sorted(v)) + "}"
    return encode(value).encode()


def read_json(path, cap=8 * 1024 * 1024):
    def pairs(values):
        result = {}
        for key, value in values:
            require(key not in result, "duplicate JSON key")
            result[key] = value
        return result

    def constant(_):
        raise InvalidScreen("nonfinite JSON number")

    def real(text):
        value = float(text)
        require(math.isfinite(value), "nonfinite JSON number")
        return value

    path = Path(path)
    require(path.stat().st_size <= cap, "JSON input exceeds byte cap")
    raw = path.read_bytes()
    require(len(raw) <= cap, "JSON input grew beyond byte cap")
    return json.loads(raw, object_pairs_hook=pairs, parse_constant=constant, parse_float=real)


def safe_name(name):
    require(isinstance(name, str) and len(name) <= 1024, "invalid source name")
    path = PurePosixPath(name)
    require(not path.is_absolute() and path.as_posix() == name and
            all(p not in (".", "..") and not p.startswith(".") for p in path.parts),
            "source name is not a public relative path")
    return name


def pointer_get(value, pointer):
    require(isinstance(pointer, str) and pointer.startswith("/"), "invalid JSON pointer")
    tokens = []
    for raw in pointer.split("/")[1:]:
        require(not re.search(r"~(?![01])", raw), "invalid JSON pointer escape")
        token = raw.replace("~1", "/").replace("~0", "~")
        tokens.append(token)
        if isinstance(value, list):
            require(re.fullmatch(r"0|[1-9][0-9]*", token) is not None and
                    int(token) < len(value), "JSON array pointer is outside original")
            value = value[int(token)]
        else:
            require(isinstance(value, dict) and token in value, "JSON pointer absent from original")
            value = value[token]
    return value, tokens


def structured(raw):
    start = b"<!-- AGENT_MEMORY_V1_BEGIN -->"
    stop = b"<!-- AGENT_MEMORY_V1_END -->"
    require(raw.count(start) == 1 and raw.count(stop) == 1, "fixture machine block is not unique")
    text = raw.split(start, 1)[1].split(stop, 1)[0]
    value = json.loads(text)
    require(value.get("schema") == "agent_memory_v2" and isinstance(value.get("items"), dict),
            "unsupported frozen fixture schema")
    return value


def load_fixture(corpus, verify_implementation=False):
    root = Path(corpus).resolve()
    freeze = read_json(root / "freeze.json", 1024 * 1024)
    manifest = read_json(root / "screen.json", 1024 * 1024)
    require(freeze.get("schema") == "agent.memory.screen.freeze.v1" and
            manifest.get("schema") == "agent.memory.screen.input.v1" and
            freeze.get("screen_id") == manifest.get("screen_id"), "screen identity/schema mismatch")
    raw_manifest = (root / "screen.json").read_bytes()
    require(freeze["manifest"]["bytes"] == len(raw_manifest) and
            freeze["manifest"]["sha256"] == digest(raw_manifest), "frozen manifest bytes changed")
    cases = manifest.get("cases")
    require(isinstance(cases, list) and len(cases) == freeze["question_count"] == 20,
            "frozen screen must contain exactly 20 cases")
    ids = [c.get("id") for c in cases]
    require(len(set(ids)) == len(ids) and all(isinstance(i, str) for i in ids), "case IDs are not distinct")
    sources = {}
    for entry in freeze["source_files"]:
        name = safe_name(entry["path"])
        require(name not in sources and not (root / name).is_symlink() and
                (root / name).resolve().is_relative_to(root), "duplicate or linked frozen source")
        raw = (root / name).read_bytes()
        require(len(raw) == entry["bytes"] and digest(raw) == entry["sha256"], "frozen source bytes changed")
        sources[name] = raw
    require(set(manifest["files"]) == set(sources), "manifest sources differ from frozen inventory")
    machine = structured(sources["STRUCTURED.md"])
    for case in cases:
        selected = case.get("files", manifest["files"])
        require(isinstance(selected, list) and bool(selected) and
                set(selected) <= set(sources), "case broadens frozen source scope")
        require(case.get("view") in ("current", "historical", "evidence"), "unknown case view")
        gold = case.get("expected_evidence")
        require(isinstance(gold, list) and bool(gold) == (case["answer_kind"] != "unanswerable"),
                "unanswerable/evidence labels disagree")
        labels = set()
        for evidence in gold:
            require(evidence["path"] in selected and isinstance(evidence.get("contains"), str) and
                    bool(evidence["contains"]), "gold label is outside case scope or empty")
            identity = (evidence["path"], evidence.get("pointer"), evidence["contains"])
            require(identity not in labels, "duplicate gold label")
            labels.add(identity)
            if "pointer" in evidence:
                original, _ = pointer_get(machine, evidence["pointer"])
            else:
                original = sources[evidence["path"]].decode()
            require(isinstance(original, str) and evidence["contains"] in original, "gold label not in original")
    require(sum(bool(c["expected_evidence"]) for c in cases) == freeze["answerable_or_conflict_or_historical_count"] and
            sum(not c["expected_evidence"] for c in cases) == freeze["unanswerable_count"], "case denominator mismatch")
    if verify_implementation:
        repo = Path(__file__).resolve().parents[1]
        for entry in freeze["implementation_files"]:
            raw = (repo / entry["path"]).read_bytes()
            require(len(raw) == entry["bytes"] and digest(raw) == entry["sha256"],
                    "retrieval implementation differs from frozen initial policy")
    return freeze, manifest, sources, machine


def validate_reference(row, sources, machine, runtime_root, source_ids, selected, view, now):
    ref = row.get("evidence")
    require(isinstance(ref, dict) and set(ref) == {"id", "schema", "path", "source_id", "revision", "span"} and
            ref["schema"] == "agent.memory.evidence.v1", "invalid original reference shape")
    path = safe_name(ref["path"])
    require(path in sources, "original reference is outside frozen corpus")
    require(path in selected, "scope exposure")
    expected_source = "memory:" + digest((str(runtime_root) + "\n" + path).encode())
    require(ref["source_id"] == source_ids[path] == expected_source, "physical source namespace mismatch")
    raw = sources[path]
    require(ref["revision"] == "sha256:" + digest(raw), "original revision mismatch")
    original_ref = {k: v for k, v in ref.items() if k != "id"}
    require(ref["id"] == "evidence:" + digest(canonical(original_ref)) and row.get("id") == ref["id"],
            "original evidence ID mismatch")
    span = ref["span"]
    require(isinstance(span, dict), "invalid original span")
    if span.get("kind") == "bytes":
        require(path != "STRUCTURED.md", "raw structured projection is ineligible")
        require(set(span) == {"kind", "begin", "end"} and type(span["begin"]) is int and type(span["end"]) is int and
                0 <= span["begin"] < span["end"] <= len(raw), "byte span outside original")
        original = raw[span["begin"]:span["end"]].decode("utf-8")
        history = raw.find(b"## Deprecated\n")
        historical = history >= 0 and span["end"] > history
    else:
        require(set(span) == {"kind", "pointer"} and span["kind"] == "json_pointer" and
                path == "STRUCTURED.md", "unsupported original pointer source")
        value, tokens = pointer_get(machine, span["pointer"])
        require(len(tokens) in (3, 5) and tokens[0] == "items" and tokens[-1] == "value" and
                isinstance(value, str), "unsupported fixture value pointer")
        record = machine["items"][tokens[1]]
        historical = len(tokens) == 5
        if historical:
            require(tokens[2] == "versions", "unsupported historical pointer")
            record = record["versions"][int(tokens[3])]
        historical = historical or record.get("status") != "active"
        for field in ("valid_from", "valid_until"):
            if field in record:
                value_time = dt.datetime.fromisoformat(record[field].replace("Z", "+00:00"))
                historical = historical or (value_time > now if field == "valid_from" else value_time <= now)
        original = tokens[1] + " = " + value
    require(isinstance(row.get("text"), str) and bool(row["text"]) and row["text"] in original,
            "context text is not addressed original text")
    require(view != "current" or not historical, "stale current exposure")
    require(type(row.get("historical")) is bool and row["historical"] == historical,
            "historical classification does not match original")
    return ref


def percentile(values, fraction):
    if not values:
        return None
    # Nearest rank, not an interpolated population estimate.
    return sorted(values)[max(0, math.ceil(len(values) * fraction) - 1)]


def evaluate(corpus, result_path, runtime_root, timing_path=None):
    freeze, manifest, sources, machine = load_fixture(corpus)
    result = read_json(result_path)
    require(result.get("schema") == "agent.memory.screen.v1" and result.get("canary") is False,
            "result is not a retrieval screen receipt")
    require(result.get("manifest_revision") == "sha256:" + digest(canonical(manifest)), "receipt manifest revision mismatch")
    runtime_root = Path(runtime_root).resolve()
    for path, raw in sources.items():
        require(not (runtime_root / path).is_symlink() and
                (runtime_root / path).resolve().is_relative_to(runtime_root) and
                (runtime_root / path).read_bytes() == raw,
                "runtime corpus differs from frozen originals")
    source_ids = {}
    for entry in result.get("sources", []):
        path = entry.get("path")
        require(path in sources and path not in source_ids and entry.get("revision") == "sha256:" + digest(sources[path]),
                "receipt source inventory mismatch")
        source_ids[path] = entry.get("source_id")
    require(set(source_ids) == set(sources), "receipt source inventory incomplete")
    now = dt.datetime.fromisoformat(freeze["frozen_at_utc"])
    cases = {c["id"]: c for c in manifest["cases"]}
    require(result.get("status") in ("prepared", "incomplete", "complete"), "unknown receipt lifecycle state")
    measured = {arm: [] for arm in ("lexical", "rerank", "expand")}
    seen = set()
    errors = []
    for arm in result.get("arms", []):
        case_id, strategy = arm.get("case"), arm.get("strategy")
        require(case_id in cases and strategy in measured and (case_id, strategy) not in seen,
                "unknown or duplicate measured arm")
        seen.add((case_id, strategy))
        case = cases[case_id]
        require(arm.get("question") == case["question"] and arm["report"].get("view") == case["view"] and
                arm["report"].get("strategy") == strategy and
                arm["report"].get("lexical_query_mode") == freeze["retrieval_policy"]["name"],
                "case question/view/strategy/policy changed")
        context = arm.get("context")
        require(isinstance(context, list) and len(context) <= freeze["retrieval_policy"]["final_records"], "context record cap exceeded")
        require(len(canonical(context)) <= freeze["retrieval_policy"]["context_bytes"], "context JSON byte cap exceeded")
        selected = set(case.get("files", manifest["files"]))
        valid = []
        context_ids = set()
        stale_scope = 0
        for row in context:
            try:
                ref = validate_reference(row, sources, machine, runtime_root, source_ids, selected, case["view"], now)
                require(ref["id"] not in context_ids, "duplicate evidence record")
                context_ids.add(ref["id"])
                require(case["view"] != "current" or not any(term in row["text"] for term in manifest["eligibility_forbidden_current"]),
                        "forbidden stale term in current context")
                valid.append(row)
            except (InvalidScreen, UnicodeError, KeyError, TypeError, ValueError) as error:
                reason = str(error)
                if reason in ("scope exposure", "original reference is outside frozen corpus",
                              "stale current exposure", "forbidden stale term in current context"):
                    stale_scope += 1
                errors.append({"case": case_id, "strategy": strategy, "reason": reason})
        gold = case["expected_evidence"]
        matched = sum(any(row["evidence"]["path"] == g["path"] and
                          ("pointer" not in g or row["evidence"]["span"].get("pointer") == g["pointer"]) and
                          g["contains"] in row["text"] for row in valid) for g in gold)
        retrieval_complete = arm.get("ok") is True and arm["report"].get("complete") is True
        ms = arm["report"].get("elapsed_ms")
        require((not retrieval_complete and ms is None) or (type(ms) is int and ms >= 0), "invalid elapsed observation")
        if retrieval_complete:
            require(type(arm["report"].get("final_records")) is int and
                    arm["report"].get("final_records") == len(context) and
                    type(arm["report"].get("context_bytes")) is int and
                    arm["report"].get("context_bytes") == len(canonical(context)), "context accounting mismatch")
        measured[strategy].append({"case": case_id, "gold_records": len(gold), "matched_records": matched,
            "evidence_recall": matched / len(gold) if gold else None,
            "complete_evidence": matched == len(gold) if gold else None,
            "returned_records": len(context), "valid_records": len(valid),
            "retrieval_complete": retrieval_complete,
            "stale_or_scope_exposure_records": stale_scope, "elapsed_ms": ms})
    aggregates = {}
    for strategy, observations in measured.items():
        bearing = [r for r in observations if r["gold_records"]]
        unknown = [r for r in observations if not r["gold_records"]]
        times = [r["elapsed_ms"] for r in observations if r["elapsed_ms"] is not None]
        aggregates[strategy] = {"observed_cases": len(observations), "expected_cases": len(cases),
            "complete_retrieval_cases": sum(r["retrieval_complete"] for r in observations),
            "evidence_bearing_cases": len(bearing),
            "macro_evidence_recall": sum(r["evidence_recall"] for r in bearing) / len(bearing) if bearing else None,
            "complete_evidence_success": sum(r["complete_evidence"] for r in bearing) / len(bearing) if bearing else None,
            "micro_matched_records": sum(r["matched_records"] for r in bearing),
            "micro_required_records": sum(r["gold_records"] for r in bearing),
            "unanswerable_cases_observed": len(unknown),
            "unanswerable_empty_context_rate": sum(r["returned_records"] == 0 for r in unknown) / len(unknown) if unknown else None,
            "stale_or_scope_exposure_records": sum(r["stale_or_scope_exposure_records"] for r in observations),
            "elapsed_ms_p50_nearest_rank": percentile(times, .5), "elapsed_ms_p95_nearest_rank": percentile(times, .95)}
    full = not errors and result.get("status") == "complete" and "in_flight" not in result and all(
        len(rows) == len(cases) and all(r["retrieval_complete"] for r in rows) for rows in measured.values())
    timing_summary = {"status": "not_measured", "scope": "index cold/warm not separated in original elapsed observations"}
    if timing_path is not None:
        timing = read_json(timing_path)
        require(timing.get("schema") == "agent.memory.lexical.timing.v1" and
                timing.get("provider_requests") == 0 and timing.get("answerer_calls") == 0 and
                timing.get("status") in ("prepared", "incomplete", "complete") and
                isinstance(timing.get("cases"), list), "invalid lexical timing receipt")
        baseline = {a["case"]: [row["id"] for row in a["context"]] for a in result["arms"] if a["strategy"] == "lexical"}
        timing_ids = set()
        for row in timing["cases"]:
            case_id = row.get("case")
            require(case_id in cases and case_id not in timing_ids and case_id in baseline and
                    row.get("view") == cases[case_id]["view"] and row.get("evidence_ids") == baseline[case_id],
                    "timing case/evidence differs from lexical baseline")
            timing_ids.add(case_id)
            for field in ("capture_ms", "cold_index_ms", "warm_index_ms"):
                number = row.get(field)
                require(type(number) in (int, float) and math.isfinite(number) and number >= 0,
                        "invalid cold/warm timing observation")
        timing_summary = {"status": "complete" if timing.get("status") == "complete" and len(timing_ids) == len(cases) else "incomplete",
            "receipt_status": timing.get("status"), "observed_cases": len(timing_ids),
            "expected_cases": len(cases), "scope": timing.get("timing_scope"),
            "input_sha256": digest(Path(timing_path).read_bytes()),
            "small_sample_caveat": "nearest-rank p50/p95 over at most 20 synthetic queries; not a population estimate or SLO"}
        for field in ("capture_ms", "cold_index_ms", "warm_index_ms"):
            values = [r[field] for r in timing["cases"]]
            timing_summary[field] = {"p50": percentile(values, .5), "p95": percentile(values, .95)}
    return {"schema": "agent.memory.screen.metrics.v1", "screen_id": manifest["screen_id"],
        "dataset_base_revision": freeze["source_revision_at_freeze"],
        "retrieval_implementation_revision": freeze["implementation_revision"],
        "in_flight": result.get("in_flight"),
        "input_identities": {"freeze_sha256": digest((Path(corpus) / "freeze.json").read_bytes()),
                             "result_sha256": digest(Path(result_path).read_bytes())},
        "validation_passed": not errors, "validation_errors": errors,
        "full_abc_screen_complete": full, "aggregates": aggregates, "cases": measured,
        "provider_usage": result.get("usage"),
        "final_answer_correctness_and_citations": {"status": "not_evaluated", "required_answerer": "existing 4B unchanged"},
        "cold_warm_timing": timing_summary,
        "ram_vram": "not measured by this offline evaluator",
        "limits": ["Synthetic screen authored by the implementer; not independently adjudicated or population representative.",
                   "Metrics cover observed contexts; incomplete contexts remain flagged and missing arms are not scored as successes.",
                   "Exact reference consistency is not execution attestation, semantic truth or supported-answer correctness.",
                   "Unanswerable cases do not add trivial successes to evidence recall/all-evidence denominators."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", required=True, type=Path)
    parser.add_argument("--results", type=Path)
    parser.add_argument("--runtime-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--timings", type=Path)
    parser.add_argument("--verify-freeze", action="store_true")
    args = parser.parse_args()
    try:
        if args.verify_freeze:
            freeze, _, _, _ = load_fixture(args.corpus, verify_implementation=True)
            value = {"freeze_valid": True, "screen_id": freeze["screen_id"], "cases": freeze["question_count"]}
        else:
            require(args.results is not None and args.runtime_root is not None, "results and runtime-root are required")
            value = evaluate(args.corpus, args.results, args.runtime_root, args.timings)
        text = json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x") as stream:
                stream.write(text)
        print(text, end="")
        return 0 if value.get("validation_passed", True) else 2
    except (OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(2, f"screen validation failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
