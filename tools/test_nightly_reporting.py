#!/usr/bin/env python3
"""Exercise the actual nightly reporting shell blocks without building any port.

Python dependencies are standard-library only. Bash and the ordinary workflow
utilities (awk, cmp, mkdir) are required. Temporary fixture outcomes are not
claims that the language runtimes passed interoperability.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = ROOT / ".github" / "workflows"
ORIGINAL_PORTS = ("c", "cs", "go", "rs", "java", "js", "dart", "elixir")
PORTS = ORIGINAL_PORTS + ("lua", "luau", "nut", "goose", "lobster", "moo")
EMBEDDED_CONTROLS = {"lua", "nut", "moo"}
EXPRESSION = re.compile(re.escape(chr(36)) + r"\{\{\s*(.*?)\s*\}\}")
MANIFEST = "Record canonical corpus manifest"
DELIVERED = "Record the delivered corpus digest"
MUTATE = "Negative control, flip one byte of one canonical record"
REPORT = "Record port interoperability report"
SUMMARY = "Render 9-language interoperability summary and JSON artifact"
SHA_STEP = "Record checked-out port commit SHA"


def run_blocks(path):
    """Read this workflow's named literal/folded/one-line run blocks.

    This is deliberately not a YAML parser. Unsupported block indicators,
    unnamed blocks, duplicate names and missing expected steps fail explicitly.
    """
    lines = path.read_text(encoding="utf-8").splitlines()
    found = {}
    name = None
    for index, line in enumerate(lines):
        match = re.match(r"\s*-\s+name:\s*(.+)$", line)
        if match:
            name = match[1]
        elif re.match(r"\s*-\s+uses:", line):
            name = None
        match = re.match(r"(\s*)run:\s*(.*)$", line)
        if not match:
            continue
        if name is None or name in found:
            raise AssertionError(f"{path}:{index + 1}: unnamed/duplicate run block")
        indent, body = len(match[1]), match[2]
        if body.startswith(("|", ">")):
            if body not in ("|", ">"):
                raise AssertionError(f"{path}:{index + 1}: unsupported block {body}")
            content = []
            for following in lines[index + 1:]:
                if following.strip() and len(following) - len(following.lstrip()) <= indent:
                    break
                if following.strip() and not following.startswith(" " * (indent + 2)):
                    raise AssertionError(f"{path}:{index + 1}: unexpected run indentation")
                content.append(following[indent + 2:] if following.strip() else "")
            body = "\n".join(content) + "\n" if body == "|" else " ".join(content) + "\n"
        elif body.startswith(("'", '"')):
            raise AssertionError(f"{path}:{index + 1}: unsupported quoted run block")
        found[name] = body
    return found


def render(body, values=None):
    values = values or {}

    def value(match):
        if match[1] not in values:
            raise AssertionError(f"missing expression fixture: {match[1]}")
        return str(values[match[1]])

    return EXPRESSION.sub(value, body)


def corpus_digest(root):
    digest = hashlib.sha256()
    for path in sorted((root / "corpus" / "conformance").glob("*.txt")):
        digest.update(path.name.encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


class NightlyReportingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bash = shutil.which("bash")
        if cls.bash is None:
            raise AssertionError("bash is required to test the workflow scripts")
        cls.blocks = run_blocks(WORKFLOWS / "nightly-diff.yml")
        for name in (MANIFEST, DELIVERED, MUTATE, REPORT, SUMMARY, SHA_STEP):
            if name not in cls.blocks:
                raise AssertionError(f"missing nightly run block: {name}")

    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory(prefix="serialize-reporting-")
        self.addCleanup(self.scratch.cleanup)
        self.temp = Path(self.scratch.name)
        self.bin = self.temp / "bin"
        self.bin.mkdir()
        # Only Git identity is stubbed. All reporting shell/Python/awk bodies are
        # extracted from the real workflow. No temporary Git commit is necessary.
        git = self.bin / "git"
        git.write_text(
            '#!/bin/sh\n[ "$#" = 2 ] && [ "$1" = rev-parse ] && '
            '[ "$2" = HEAD ] || exit 97\n'
            "printf '%s\\n' 0123456789012345678901234567890123456789\n",
            encoding="utf-8",
        )
        git.chmod(0o755)
        self.sequence = 0

    def directory(self, label):
        self.sequence += 1
        root = self.temp / f"{self.sequence}-{label}"
        root.mkdir()
        return root

    def execute(self, name, root, values=None, env=None, body=None, expected=0):
        script = render(self.blocks[name] if body is None else body, values)
        environment = dict(os.environ)
        environment["PATH"] = str(self.bin) + os.pathsep + environment.get("PATH", "")
        environment.update(env or {})
        result = subprocess.run(
            [self.bash, "-e", "-o", "pipefail", "-c", script],
            cwd=root, env=environment, capture_output=True, text=True,
        )
        self.assertEqual(
            result.returncode, expected,
            f"{name}\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}",
        )
        return result

    def manifest(self, negative=False):
        root = self.directory("mutated" if negative else "normal")
        shutil.copy2(ROOT / "COMPATIBILITY.txt", root / "COMPATIBILITY.txt")
        corpus = root / "corpus" / "conformance"
        corpus.mkdir(parents=True)
        committed_count = 0
        for path in sorted((ROOT / "conformance").glob("*.txt")):
            data = path.read_text(encoding="utf-8")
            committed_count += sum(line.startswith("operation ") for line in data.splitlines())
            shutil.copy2(path, corpus / path.name)
        self.assertGreater(committed_count, 0)
        # A legal comment directly before the record used to make the entire
        # paragraph disappear from the reported count.
        (corpus / "diff.txt").write_text(
            "# comment immediately before a record\n"
            "operation bits\nname nightly-reporting-fixture\nparam bits = 1\n"
            "bytes 00\nexpect value = 0\nconsumed 1\nwriter canonical\n\n",
            encoding="utf-8",
        )
        self.execute(MANIFEST, root, {
            "steps.seed.outputs.seed": "1",
            "inputs.count || '5000'": "1",
            "inputs.negative_control": "true" if negative else "false",
        })
        metadata = json.loads((root / "corpus" / "meta.json").read_text())
        self.assertEqual(metadata["committed_records"], committed_count)
        self.assertEqual(metadata["generated_records"], 1)
        self.assertEqual(metadata["corpus_sha256"], corpus_digest(root))
        if negative:
            self.execute(MUTATE, root)
        self.execute(DELIVERED, root)
        delivered = json.loads((root / "corpus" / "meta.json").read_text())
        self.assertEqual(delivered["corpus_sha256"], metadata["corpus_sha256"])
        self.assertEqual(delivered["delivered_corpus_sha256"], corpus_digest(root))
        self.assertEqual(
            delivered["corpus_sha256"] != delivered["delivered_corpus_sha256"], negative,
        )
        return root, delivered

    def report(self, port, outcome, status, body=None):
        root = self.directory(port)
        steps = {} if outcome is None else {f"run_{port}": {"outcome": outcome}}
        self.execute(REPORT, root, {
            "matrix.port": port,
            "job.status": status,
            "steps.row.outputs.release": "fixture-release",
            "steps.port_sha.outputs.commit": "fixture-commit",
        }, {"STEP_RESULTS": json.dumps(steps)}, body=body)
        return json.loads((root / "report" / f"port-{port}.json").read_text())

    def assert_report(self, record, port, outcome):
        verdict = "passed" if outcome == "success" else "not-established"
        self.assertEqual(record["reader_canonical_and_diff"], verdict)
        self.assertEqual(record["writer_canonical_byte_identity"], verdict)
        self.assertEqual(
            record["negative_controls"],
            verdict if port in EMBEDDED_CONTROLS else "not-established",
        )

    def summary(self, negative=False, missing=None, failed=None):
        root, _ = self.manifest(negative)
        reports = root / "reports"
        reports.mkdir()
        for port in ORIGINAL_PORTS:
            if port == missing:
                continue
            record = self.report(
                port, None if port == failed else "success",
                "failure" if port == failed else "success",
            )
            (reports / f"port-{port}.json").write_text(json.dumps(record))
        self.execute(SUMMARY, root, env={"GITHUB_STEP_SUMMARY": str(root / "summary.md")})
        return json.loads((root / "summary" / "interoperability.json").read_text())

    def test_workflow_bash_syntax(self):
        count = 0
        for filename in ("nightly-diff.yml", "family-matrix.yml"):
            for name, body in run_blocks(WORKFLOWS / filename).items():
                with self.subTest(workflow=filename, step=name):
                    values = {match[1]: "1" for match in EXPRESSION.finditer(body)}
                    result = subprocess.run(
                        [self.bash, "-n"], input=render(body, values),
                        capture_output=True, text=True,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    count += 1
        self.assertGreater(count, 0)

    def test_quote_negative_control(self):
        body = self.blocks[SHA_STEP]
        anchor = '($sha)"'
        self.assertEqual(body.count(anchor), 1, "quote mutation must touch exactly one echo")
        mutant = body.replace(anchor, anchor + '"')
        values = {match[1]: "1" for match in EXPRESSION.finditer(mutant)}
        result = subprocess.run(
            [self.bash, "-n"], input=render(mutant, values), capture_output=True, text=True,
        )
        self.assertNotEqual(result.returncode, 0, "the original unmatched quote must fail")

    def test_counts_and_normal_delivered_digest(self):
        self.manifest()

    def test_mutated_delivery_preserves_certified_digest(self):
        self.manifest(negative=True)

    def test_missing_or_unrequested_mutation_fails(self):
        for negative in (False, True):
            with self.subTest(negative=negative):
                root, metadata = self.manifest(negative)
                metadata["negative_control"] = not negative
                (root / "corpus" / "meta.json").write_text(json.dumps(metadata))
                self.execute(DELIVERED, root, expected=1)

    def test_actual_port_outcomes(self):
        for port in PORTS:
            for outcome, status in (
                ("success", "success"), (None, "failure"),
                ("failure", "failure"), ("success", "failure"),
            ):
                with self.subTest(port=port, outcome=outcome, status=status):
                    self.assert_report(self.report(port, outcome, status), port, outcome)

    def test_omitted_control_false_pass_negative_control(self):
        body = self.blocks[REPORT]
        # Extracted bodies are de-indented; retain the exact current expression
        # as the anchor so a workflow change cannot silently make this a no-op.
        lines = [line for line in body.splitlines() if line.startswith("controls = ")]
        self.assertEqual(len(lines), 1, "missing control-verdict assignment")
        mutant = body.replace(lines[0], "controls = verdict")
        record = self.report("go", "success", "success", body=mutant)
        self.assertEqual(record["negative_controls"], "passed")
        with self.assertRaises(AssertionError):
            self.assert_report(record, "go", "success")

    def test_full_nine_and_missing_or_failed_runner(self):
        for missing, failed, expected in ((None, None, True), ("go", None, False), (None, "go", False)):
            with self.subTest(missing=missing, failed=failed):
                result = self.summary(missing=missing, failed=failed)
                self.assertEqual(result["all_original_nine_passed"], expected)
                self.assertEqual(len(result["original_nine"]), 9)
                if missing:
                    go = next(row for row in result["original_nine"] if row["port"] == missing)
                    self.assertEqual(go["status"], "missing")
                    self.assertEqual(go["reader_canonical_and_diff"], "not-established")
                    self.assertTrue(any(
                        gap["port"] == missing and "No port report" in gap["gap"]
                        for gap in result["explicit_gaps"]
                    ))

    def test_negative_dispatch_never_certifies_all_nine(self):
        result = self.summary(negative=True)
        self.assertFalse(result["all_original_nine_passed"])
        self.assertEqual(len(result["original_nine"]), 9)


if __name__ == "__main__":
    unittest.main(verbosity=2)
