#!/usr/bin/env python3
"""Tests for the disc-layout planner. Run: python tools/disc/test_layout_planner.py"""

import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import layout_planner as lp


class ParseTrace(unittest.TestCase):
    def test_scrapes_a_pcsx2_log_and_ignores_everything_else(self):
        log = (
            "[m10] io sample starting\n"
            "M10_TRACE 3 entries:\n"
            "M10_TRACE   0 host:boot.p2b\n"
            "M10_TRACE   1 host:level1.p2b\n"
            "some other line mentioning level9.p2b\n"
            "M10_TRACE   2 host:music.adp\n"
        )
        self.assertEqual(lp.parse_trace(log),
                         ["boot.p2b", "level1.p2b", "music.adp"])

    def test_sees_through_the_pcsx2_timestamp_prefix(self):
        # This is the form the tool is actually handed: run-emu-test.sh saves
        # PCSX2's console log, and every line carries a timestamp.
        log = ("[    1.6174] M10_TRACE 3 entries:\n"
               "[    1.6175] M10_TRACE   0 host:m5-scene.p2b\n"
               "[    1.6175] M10_TRACE   1 host:skinscene.p2b\n"
               "[    1.6176] M10_TRACE   2 host:spin-scene.p2b\n")
        self.assertEqual(lp.parse_trace(log),
                         ["m5-scene.p2b", "skinscene.p2b", "spin-scene.p2b"])

    def test_reads_a_raw_dump_when_there_are_no_log_markers(self):
        dump = "boot.p2b\nlevel1.p2b\n\n# a comment\nmusic.adp\n"
        self.assertEqual(lp.parse_trace(dump),
                         ["boot.p2b", "level1.p2b", "music.adp"])

    def test_strips_device_prefixes_and_iso_version_suffixes(self):
        self.assertEqual(lp.basename_on_media("cdrom0:\\LEVEL2.P2B;1"),
                         "LEVEL2.P2B")
        self.assertEqual(lp.basename_on_media("host:assets/b.p2b"), "b.p2b")
        self.assertEqual(lp.basename_on_media("mass:x.adp"), "x.adp")
        self.assertEqual(lp.basename_on_media("plain.p2b"), "plain.p2b")

    def test_log_prose_is_not_mistaken_for_a_raw_dump_of_paths(self):
        # A log with no M10_TRACE lines must yield nothing, not a layout
        # built out of sentences.
        self.assertEqual(lp.parse_trace("[m10] nothing happened\n"
                                        "boot ok, 3 pads found\n"), [])

    def test_a_repeated_access_is_recorded_once_at_its_first_position(self):
        dump = "a.p2b\nb.p2b\na.p2b\nc.p2b\n"
        self.assertEqual(lp.parse_trace(dump), ["a.p2b", "b.p2b", "c.p2b"])


class PlanOrder(unittest.TestCase):
    def setUp(self):
        self.staged = [("a.p2b", 100), ("boot.elf", 50), ("c.p2b", 100),
                       ("z.p2b", 100)]

    def test_traced_files_lead_in_access_order_and_the_rest_follow(self):
        ordered, missing = lp.plan_order(["z.p2b", "a.p2b"], self.staged)
        self.assertEqual(ordered, ["z.p2b", "a.p2b", "boot.elf", "c.p2b"])
        self.assertEqual(missing, [])

    def test_the_boot_elf_goes_first_even_though_no_trace_can_see_it(self):
        ordered, _ = lp.plan_order(["z.p2b"], self.staged, boot_elf="boot.elf")
        self.assertEqual(ordered[0], "boot.elf")
        self.assertEqual(ordered[1], "z.p2b")

    def test_case_folds_because_iso_9660_upper_cases_names(self):
        ordered, missing = lp.plan_order(["Z.P2B"], self.staged)
        self.assertEqual(missing, [])
        self.assertEqual(ordered[0], "z.p2b")

    def test_a_traced_file_that_is_not_staged_is_reported_not_dropped(self):
        ordered, missing = lp.plan_order(["ghost.p2b", "a.p2b"], self.staged)
        self.assertEqual(missing, ["ghost.p2b"])
        self.assertEqual(ordered[0], "a.p2b")
        # Every staged file still lands somewhere.
        self.assertEqual(sorted(ordered), sorted(n for n, _ in self.staged))


class SeekCost(unittest.TestCase):
    def test_reading_in_layout_order_costs_less_than_reading_backwards(self):
        sizes = {"a": 2048, "b": 2048, "c": 2048}
        forward = lp.seek_cost(["a", "b", "c"], sizes, ["a", "b", "c"])
        backward = lp.seek_cost(["c", "b", "a"], sizes, ["a", "b", "c"])
        self.assertLess(forward, backward)

    def test_a_perfectly_planned_layout_never_seeks_backwards(self):
        sizes = {"a": 4096, "b": 2048, "c": 8192}
        traced = ["c", "a", "b"]
        ordered, _ = lp.plan_order(traced, sorted(sizes.items()))
        self.assertEqual(lp.seek_cost(ordered, sizes, traced), 0)

    def test_files_outside_the_layout_are_ignored_rather_than_counted(self):
        sizes = {"a": 2048}
        self.assertEqual(lp.seek_cost(["a"], sizes, ["a", "not-on-disc"]), 0)


class WriteXml(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ps2ur-layout-")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_the_file_elements_come_out_in_the_planned_order(self):
        out = os.path.join(self.dir, "disc.xml")
        lp.write_xml(out, ["boot.elf", "level1.p2b", "level2.p2b"], "stage",
                     "game.iso", "SLPS-99999", "UNITYPS2", 0)
        with open(out, "r", encoding="ascii") as f:
            text = f.read()
        self.assertLess(text.index("boot.elf"), text.index("level1.p2b"))
        self.assertLess(text.index("level1.p2b"), text.index("level2.p2b"))
        self.assertIn('<iso_project image_name="game.iso"', text)
        self.assertIn('serial="SLPS-99999"', text)

    def test_names_too_long_for_iso_9660_are_reported(self):
        out = os.path.join(self.dir, "disc.xml")
        long_name = "a-name-that-is-definitely-longer-than-31.p2b"
        reported = lp.write_xml(out, ["ok.p2b", long_name], "stage", "g.iso",
                                "SLPS-99999", "UNITYPS2", 0)
        self.assertEqual(reported, [long_name])


class EndToEnd(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="ps2ur-layout-e2e-")
        self.stage = os.path.join(self.dir, "stage")
        os.makedirs(self.stage)
        for name, size in [("boot.elf", 4096), ("a.p2b", 8192),
                           ("m.p2b", 2048), ("z.p2b", 6144)]:
            with open(os.path.join(self.stage, name), "wb") as f:
                f.write(b"\0" * size)
        self.trace = os.path.join(self.dir, "run.log")
        with open(self.trace, "w", encoding="ascii") as f:
            f.write("M10_TRACE 3 entries:\n"
                    "M10_TRACE   0 host:z.p2b\n"
                    "M10_TRACE   1 host:m.p2b\n"
                    "M10_TRACE   2 host:a.p2b\n")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_plans_and_writes_a_usable_script(self):
        out = os.path.join(self.dir, "disc.xml")
        rc = lp.main(["--trace", self.trace, "--stage", self.stage,
                      "--elf", "boot.elf", "--output", out])
        self.assertEqual(rc, 0)
        with open(out, "r", encoding="ascii") as f:
            text = f.read()
        order = [line.split('name="')[1].split('"')[0]
                 for line in text.splitlines() if "<file name=" in line]
        self.assertEqual(order, ["boot.elf", "z.p2b", "m.p2b", "a.p2b"])

    def test_a_missing_staging_directory_is_an_error_not_a_traceback(self):
        rc = lp.main(["--trace", self.trace, "--stage",
                      os.path.join(self.dir, "nope"), "--report-only"])
        self.assertEqual(rc, 2)

    def test_a_trace_with_no_accesses_is_an_error(self):
        empty = os.path.join(self.dir, "empty.log")
        with open(empty, "w", encoding="ascii") as f:
            f.write("[m10] nothing happened\n")
        rc = lp.main(["--trace", empty, "--stage", self.stage, "--report-only"])
        self.assertEqual(rc, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
