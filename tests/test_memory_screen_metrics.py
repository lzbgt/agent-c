#!/usr/bin/env python3
"""Native/offline integration plus adversarial reference and denominator checks."""
import copy
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tools"))
import memory_screen_metrics as metrics

BINARY = Path(sys.argv.pop(1)).resolve()
TIMING_BINARY = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
FIXTURE = REPO / "tests/fixtures/memory_screen_v1"


class ScreenMetrics(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="memory-screen-metrics-")
        cls.root = Path(cls.temp.name)
        cls.runtime = cls.root / "corpus"
        cls.runtime.mkdir()
        cls.freeze, cls.manifest, cls.sources, cls.machine = metrics.load_fixture(FIXTURE)
        for name, raw in cls.sources.items():
            (cls.runtime / name).parent.mkdir(parents=True, exist_ok=True)
            (cls.runtime / name).write_bytes(raw)
        cls.original_path = cls.root / "native.json"
        run = subprocess.run([str(BINARY), "--memory-root", str(cls.runtime), "--manifest",
            str(FIXTURE / "screen.json"), "--lexical-only", "--output", str(cls.original_path)],
            capture_output=True, text=True, timeout=15)
        if run.returncode:
            raise RuntimeError("native free lexical fixture failed: " + run.stderr)
        cls.original = metrics.read_json(cls.original_path)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def evaluate(self, receipt):
        path = self.root / "mutated.json"
        path.write_bytes(metrics.canonical(receipt))
        return metrics.evaluate(FIXTURE, path, self.runtime)

    def reference(self, name, *, pointer=None, begin=None, end=None):
        source = "memory:" + hashlib.sha256((str(self.runtime.resolve()) + "\n" + name).encode()).hexdigest()
        ref = {"schema": "agent.memory.evidence.v1", "path": name, "source_id": source,
               "revision": "sha256:" + hashlib.sha256(self.sources[name]).hexdigest()}
        ref["span"] = {"kind": "json_pointer", "pointer": pointer} if pointer else {
            "kind": "bytes", "begin": begin, "end": end}
        ref["id"] = "evidence:" + hashlib.sha256(metrics.canonical(ref)).hexdigest()
        return ref

    def row(self, key, *, version=None, historical=False):
        token = key.replace("~", "~0").replace("/", "~1")
        pointer = "/items/" + token + (f"/versions/{version}" if version is not None else "") + "/value"
        value, _ = metrics.pointer_get(self.machine, pointer)
        ref = self.reference("STRUCTURED.md", pointer=pointer)
        return {"id": ref["id"], "evidence": ref, "text": key + " = " + value,
                "score": .1, "historical": historical}

    def arm(self, case_id, context, complete=True):
        case = next(c for c in self.manifest["cases"] if c["id"] == case_id)
        return {"case": case_id, "question": case["question"], "strategy": "lexical", "ok": complete,
            "context": context, "report": {"strategy": "lexical", "view": case["view"],
            "lexical_query_mode": "question_terms_v1", "complete": complete, "final_records": len(context),
            "context_bytes": len(metrics.canonical(context)), "elapsed_ms": 1}}

    def receipt(self, arms):
        receipt = copy.deepcopy(self.original)
        receipt["status"] = "incomplete"
        receipt["arms"] = arms
        return receipt

    def test_native_reference_interoperability(self):
        result = metrics.evaluate(FIXTURE, self.original_path, self.runtime)
        self.assertTrue(result["validation_passed"], result["validation_errors"])
        a = result["aggregates"]["lexical"]
        self.assertEqual((a["observed_cases"], a["complete_retrieval_cases"], a["evidence_bearing_cases"]), (20, 20, 16))
        self.assertEqual(a["micro_required_records"], 28)
        self.assertEqual(a["unanswerable_cases_observed"], 4)
        self.assertFalse(result["full_abc_screen_complete"])
        self.assertIsNone(result["aggregates"]["rerank"]["macro_evidence_recall"])
        self.assertIsNone(result["provider_usage"])
        self.assertEqual(result["final_answer_correctness_and_citations"]["status"], "not_evaluated")

    def test_denominators_and_partial_coverage(self):
        receipt = self.receipt([self.arm("q01", [self.row("drive.product")]),
                                self.arm("q08", [self.row("spectra.board")]), self.arm("q14", [])])
        result = self.evaluate(receipt)
        a = result["aggregates"]["lexical"]
        self.assertTrue(result["validation_passed"])
        self.assertEqual(a["observed_cases"], 3)
        self.assertEqual(a["evidence_bearing_cases"], 2)
        self.assertAlmostEqual(a["macro_evidence_recall"], 2 / 3)
        self.assertEqual(a["complete_evidence_success"], .5)
        self.assertEqual((a["micro_matched_records"], a["micro_required_records"]), (2, 4))
        self.assertEqual(a["unanswerable_empty_context_rate"], 1)
        self.assertFalse(result["full_abc_screen_complete"])
        receipt["status"] = "prepared"
        receipt["in_flight"] = {"case": "q02", "strategy": "rerank"}
        receipt["arms"][0]["ok"] = False
        receipt["arms"][0]["report"]["complete"] = False
        receipt["arms"][0]["report"].pop("elapsed_ms")
        result = self.evaluate(receipt)
        self.assertEqual(result["aggregates"]["lexical"]["complete_retrieval_cases"], 2)
        self.assertEqual(result["in_flight"], receipt["in_flight"])

    def test_tampered_reference_or_text_never_earns_credit(self):
        for failure in ("revision", "namespace", "id", "pointer", "text", "raw_structured", "oversized_span"):
            with self.subTest(failure=failure):
                row = self.row("drive.product")
                if failure == "revision":
                    row["evidence"]["revision"] = "sha256:" + "0" * 64
                elif failure == "namespace":
                    row["evidence"]["source_id"] = "memory:" + "0" * 64
                elif failure == "id":
                    row["id"] = "evidence:" + "0" * 64
                elif failure == "pointer":
                    row["evidence"]["span"]["pointer"] = "/items/missing/value"
                elif failure == "text":
                    row["text"] = "invented DRIVE_S5 answer"
                else:
                    row["evidence"]["span"] = {"kind": "bytes", "begin": 0,
                        "end": len(self.sources["STRUCTURED.md"]) if failure == "raw_structured" else 10**9}
                # Rehash coherent forgeries too: a valid ID alone must not establish original support.
                if failure != "id":
                    base = {k: v for k, v in row["evidence"].items() if k != "id"}
                    row["evidence"]["id"] = "evidence:" + metrics.digest(metrics.canonical(base))
                    row["id"] = row["evidence"]["id"]
                result = self.evaluate(self.receipt([self.arm("q01", [row])]))
                self.assertFalse(result["validation_passed"])
                self.assertEqual(result["aggregates"]["lexical"]["micro_matched_records"], 0)

    def test_scope_staleness_and_future_state_exposure(self):
        rows = [self.row("terra.release", version=0, historical=False),
                self.row("kappa.retired", historical=False), self.row("future.release", historical=False)]
        for row in rows:
            result = self.evaluate(self.receipt([self.arm("q14", [row])]))
            self.assertFalse(result["validation_passed"])
            self.assertEqual(result["aggregates"]["lexical"]["stale_or_scope_exposure_records"], 1)
        name = "teams/other.md"
        ref = self.reference(name, begin=0, end=len(self.sources[name]))
        row = {"id": ref["id"], "evidence": ref, "text": self.sources[name].decode(), "historical": False}
        result = self.evaluate(self.receipt([self.arm("q15", [row])]))
        self.assertFalse(result["validation_passed"])
        self.assertEqual(result["aggregates"]["lexical"]["stale_or_scope_exposure_records"], 1)
        ref = self.reference("MEMORY.md", begin=0, end=len(self.sources["MEMORY.md"]) + 1)
        row = {"id": ref["id"], "evidence": ref, "text": "Borealis-Lot9", "historical": False}
        result = self.evaluate(self.receipt([self.arm("q07", [row])]))
        self.assertFalse(result["validation_passed"])
        self.assertEqual(result["aggregates"]["lexical"]["micro_matched_records"], 0)

    def test_duplicate_evidence_and_changed_case_protocol(self):
        row = self.row("drive.product")
        result = self.evaluate(self.receipt([self.arm("q01", [row, row])]))
        self.assertFalse(result["validation_passed"])
        self.assertEqual(result["aggregates"]["lexical"]["micro_matched_records"], 1)
        for mutation in ("question", "view", "strategy", "manifest", "inventory", "elapsed", "count", "boolean_count"):
            with self.subTest(mutation=mutation):
                receipt = self.receipt([self.arm("q01", [row])])
                arm = receipt["arms"][0]
                if mutation == "question": arm["question"] = "different query"
                elif mutation == "view": arm["report"]["view"] = "historical"
                elif mutation == "strategy": arm["report"]["strategy"] = "expand"
                elif mutation == "manifest": receipt["manifest_revision"] = "sha256:" + "0" * 64
                elif mutation == "inventory": receipt["sources"].pop()
                elif mutation == "elapsed": arm["report"]["elapsed_ms"] = True
                elif mutation == "count": arm["report"]["final_records"] = 7
                elif mutation == "boolean_count": arm["report"]["final_records"] = True
                with self.assertRaises(metrics.InvalidScreen): self.evaluate(receipt)

    def test_frozen_runtime_bytes_and_strict_json(self):
        path = self.runtime / "MEMORY.md"
        raw = path.read_bytes()
        try:
            path.write_bytes(raw + b"changed")
            with self.assertRaises(metrics.InvalidScreen): self.evaluate(self.original)
        finally:
            path.write_bytes(raw)
        for text in ('{"a":1,"a":2}', '{"a":NaN}', '{"a":1e400}'):
            path = self.root / "bad.json"
            path.write_text(text)
            with self.assertRaises(metrics.InvalidScreen): metrics.read_json(path)
        self.assertEqual(metrics.canonical({"score": .1}), b'{"score":0.10000000000000001}')
        self.assertEqual(metrics.percentile([1, 2, 3, 4], .95), 4)
        self.assertIsNone(metrics.percentile([], .5))

    def test_cold_warm_index_timing_same_evidence(self):
        if TIMING_BINARY is None:
            self.skipTest("timing binary not supplied")
        output = self.root / "timing.json"
        run = subprocess.run([str(TIMING_BINARY), "--memory-root", str(self.runtime), "--manifest",
            str(FIXTURE / "screen.json"), "--output", str(output)], capture_output=True, text=True, timeout=15)
        self.assertEqual(run.returncode, 0, run.stderr)
        timing = metrics.read_json(output)
        self.assertEqual(timing["status"], "complete")
        self.assertEqual(len(timing["cases"]), 20)
        self.assertEqual(timing["provider_requests"], 0)
        baseline = {a["case"]: [row["id"] for row in a["context"]] for a in self.original["arms"]}
        for case in timing["cases"]:
            self.assertEqual(case["evidence_ids"], baseline[case["case"]])
            for field in ("capture_ms", "cold_index_ms", "warm_index_ms"):
                self.assertGreaterEqual(case[field], 0)
        report = metrics.evaluate(FIXTURE, self.original_path, self.runtime, output)
        self.assertEqual(report["cold_warm_timing"]["observed_cases"], 20)
        self.assertEqual(report["cold_warm_timing"]["status"], "complete")
        timing["cases"][0]["evidence_ids"] = []
        output.write_bytes(metrics.canonical(timing))
        with self.assertRaises(metrics.InvalidScreen):
            metrics.evaluate(FIXTURE, self.original_path, self.runtime, output)


if __name__ == "__main__":
    unittest.main()
