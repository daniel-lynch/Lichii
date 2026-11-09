#!/usr/bin/env python3
"""Socket-free tests; importing the harness does not launch it."""
import importlib.util
from pathlib import Path
import tempfile
import sys
sys.dont_write_bytecode = True
import unittest
from unittest.mock import patch


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


checker = load("check_logs")
harness = load("two_instances")
READY = "OoT co-op: relay connected\nOoT co-op: baseline exported\nOoT co-op: baseline exchanged\n"
OBSERVED = "OoT co-op: observed class=bit key=0000000000000065 value=1 path=observer\n"
APPLIED = "OoT co-op: applied class=bit key=0000000000000065 value=1 path=setter echo=1-bit-0000000000000065\n"
SUPPRESSED = "OoT co-op: suppressed class=bit key=0000000000000065 value=1 path=observer echo=1-bit-0000000000000065\n"


class HarnessTests(unittest.TestCase):
    def test_positive_receipt(self):
        self.assertTrue(all(checker.check(READY + OBSERVED, READY + SUPPRESSED + APPLIED).values()))

    def test_silence_does_not_prove_no_echo(self):
        self.assertFalse(checker.check(READY + OBSERVED, READY + APPLIED)["no echo"])

    def test_baseline_must_precede_grant(self):
        self.assertFalse(checker.check(OBSERVED + READY, READY + APPLIED + SUPPRESSED)["baseline before grant"])

    def test_echo_fails(self):
        self.assertFalse(checker.check(READY + OBSERVED, READY + APPLIED + SUPPRESSED + OBSERVED)["no echo"])

    def test_unrelated_receipt_fails(self):
        self.assertFalse(checker.check(READY + OBSERVED,
                                      READY + (APPLIED + SUPPRESSED).replace("0065", "0066"))["applied on B"])

    def test_duplicate_apply_and_error_fail(self):
        self.assertFalse(checker.check(READY + OBSERVED, READY + APPLIED * 2 + SUPPRESSED)["applied on B"])
        self.assertFalse(checker.check(READY + OBSERVED,
                                      READY + APPLIED + SUPPRESSED + "OoT co-op: quarantined")["no runtime failures"])

    def test_policy_refusal_is_not_a_runtime_failure(self):
        refusal = "OoT co-op: not applied: class/key policy disabled or queue full\n"
        self.assertTrue(checker.check(READY + OBSERVED, READY + refusal)["no runtime failures"])
        for line in ("OoT co-op: pending queue full; reconnect for a fresh snapshot\n",
                     "OoT co-op: outgoing queue full; observation skipped\n"):
            self.assertFalse(checker.check(READY + OBSERVED, READY + line)["no runtime failures"])

    def test_suppression_epoch_must_match(self):
        self.assertFalse(checker.check(READY + OBSERVED,
                                      READY + APPLIED + SUPPRESSED.replace("echo=1", "echo=2"))["no echo"])

    def test_refuses_process_and_priority(self):
        for running, marker in ((0, False), (1, True), (2, False)):
            with patch.object(harness.subprocess, "run") as run, patch.object(harness, "PRIORITY") as priority:
                run.return_value.returncode = running
                run.return_value.stdout = "42\n"
                priority.exists.return_value = marker
                with self.assertRaises(RuntimeError):
                    harness.refuse_busy()

    def test_empty_allow_control(self):
        self.assertTrue(all(checker.check(READY, "", allow="").values()))
        for event in ("started", "returned", "applied", "suppressed", "observed", "baseline"):
            self.assertFalse(all(checker.check(READY, APPLIED.replace("applied", event), allow="").values()))

    def test_bool_receipts(self):
        self.assertTrue(all(checker.check((READY + OBSERVED).replace("bit", "bool"),
                                         (READY + APPLIED + SUPPRESSED).replace("bit", "bool"),
                                         allow="bool").values()))

    def test_vram_profile(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd() / ".tmp") as directory:
            root = Path(directory)
            source = root / "source"
            (source / "config/custom").mkdir(parents=True)
            for file in ("qt-config.ini", "custom/game.ini"):
                (source / "config" / file).write_text("[Renderer]\nresolution_setup=2\nvram_usage_mode=1\n[System]\nuse_docked_mode=1\n")
            for name in ("A", "B"):
                dest = root / name
                harness.prepare_profile(source, dest, vram_profile=True)
                for file in ("qt-config.ini", "custom/game.ini"):
                    text = (dest / "config" / file).read_text()
                    for key in ("resolution_setup", "vram_usage_mode", "use_docked_mode"):
                        self.assertIn(key + "=0", text)
                        self.assertIn(key + "\\default=false", text)
                        self.assertIn(key + "\\use_global=false", text)
            self.assertIn("resolution_setup=2", (source / "config/qt-config.ini").read_text())

    def test_profile_paths_are_private_copies(self):
        scratch = Path.cwd() / ".tmp"
        with tempfile.TemporaryDirectory(dir=scratch) as directory:
            root = Path(directory)
            source, dest = root / "source", root / "A/user"
            (source / "config").mkdir(parents=True)
            (source / "nand").mkdir()
            save = source / "nand/save.bin"
            save.write_bytes(b"untouched")
            (source / "config/qt-config.ini").write_text(
                "[Data%20Storage]\nsave_directory=/original/save\nnand_directory=/original/nand\n")
            (source / "config/custom").mkdir()
            (source / "config/custom/game.ini").write_text("[WebService]\neden_token=SECRETVALUE\n")
            with (source / "config/qt-config.ini").open("a") as stream:
                stream.write("[WebService]\neden_username=someone\neden_token=SECRETVALUE\n"
                             "eden_token\\default=false\n")
            harness.prepare_profile(source, dest)
            text = (dest / "config/qt-config.ini").read_text()
            for ini in ("qt-config.ini", "custom/game.ini"):
                copied = (dest / "config" / ini).read_text()
                self.assertNotIn("SECRETVALUE", copied)
                self.assertIn("eden_token=\n", copied)
            self.assertIn("eden_username=someone", text)
            self.assertIn("SECRETVALUE", (source / "config/qt-config.ini").read_text())
            self.assertNotIn("/original", text)
            self.assertIn(str(dest / "nand"), text)
            (dest / "nand/save.bin").write_bytes(b"changed")
            self.assertEqual(save.read_bytes(), b"untouched")


if __name__ == "__main__":
    unittest.main()
