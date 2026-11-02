"""CLI regressions; requires a built unicorn, Python 3, and samtools on PATH."""
import os
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
UNICORN = os.environ.get("UNICORN_TEST_BINARY", str(ROOT / "unicorn"))


def run(*args):
    if str(args[0]) == str(UNICORN) and "--outstat" in args:
        args = (*args, "--plain-text")
    result = subprocess.run([str(x) for x in args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
    if result.returncode:
        raise AssertionError(f"Command {args} returned {result.returncode}:\n{result.stderr}")
    return result


def table(text):
    lines = text.strip().splitlines()
    if len(lines) < 2:
        raise AssertionError("statistics must contain a header and data rows")
    return {row.split("\t")[0]: row.split("\t") for row in lines[1:]}


class Outputs(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("samtools"):
            raise RuntimeError("samtools is required for output regression tests")
        cls.temp = tempfile.TemporaryDirectory(prefix="unicorn-tests-")
        cls.path = Path(cls.temp.name)
        # These distinct names have the same 32-bit kh_hash_str value.
        first, second = "read79b3d9f3f6a4854a", "reade1b024b95172fb60"
        seq = "ACGT" * 8
        sam = "@HD\tVN:1.6\tSO:queryname\tSS:queryname:natural\n"
        sam += "@SQ\tSN:refA\tLN:1000\n@SQ\tSN:refB\tLN:2000\n"
        for name, flag, ref, pos in [(first, 0, "refA", 11),
                                      (first, 256, "refA", 81),
                                      (first, 256, "refB", 21),
                                      (second, 0, "refA", 161)]:
            sam += f"{name}\t{flag}\t{ref}\t{pos}\t60\t32M\t*\t0\t0\t{seq}\t{'I'*32}\tNM:i:0\tAS:i:0\tXT:i:2\tXR:i:2\n"
        source = cls.path / "input.sam"
        source.write_text(sam)
        cls.query = cls.path / "query.bam"
        cls.coord = cls.path / "coordinate.bam"
        run("samtools", "view", "-b", "-o", cls.query, source)
        run("samtools", "sort", "-o", cls.coord, cls.query)
        cls.names = cls.path / "names.dmp"
        cls.nodes = cls.path / "nodes.dmp"
        cls.acc2tax = cls.path / "acc2tax.tsv"
        cls.names.write_text("1\t|\troot\t|\t\t|\tscientific name\t|\n"
                             "2\t|\tTest genus\t|\t\t|\tscientific name\t|\n")
        cls.nodes.write_text("1\t|\t1\t|\tno rank\t|\n2\t|\t1\t|\tgenus\t|\n")
        cls.acc2tax.write_text("accession\taccession.version\ttaxid\tgi\n"
                               "refA\trefA\t2\t0\nrefB\trefB\t2\t0\n")

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_refstats_stdout_and_file_match(self):
        stdout = run(UNICORN, "refstats", "-b", self.query).stdout
        output = self.path / "stats.tsv"
        run(UNICORN, "refstats", "-b", self.query, "--outstat", output)
        self.assertEqual(table(stdout), table(output.read_text()))
        self.assertEqual(set(table(stdout)), {"refA", "refB"})

    def test_filtered_bam_is_complete_and_matches_retained_reference(self):
        # Both names collide under the legacy 32-bit name hash.
        for name, source in [("query", self.query), ("coord", self.coord)]:
            with self.subTest(order=name):
                output = self.path / f"{name}.filtered.bam"
                result = run(UNICORN, "refstats", "-b", source, "-o", output)
                self.assertEqual(set(table(result.stdout)), {"refA", "refB"})
                run("samtools", "quickcheck", output)
                self.assertEqual(int(run("samtools", "view", "-c", output).stdout), 4)
                decoded = run("samtools", "view", output).stdout.splitlines()
                self.assertEqual(len(decoded), 4)
                self.assertEqual({row.split("\t")[2] for row in decoded}, {"refA", "refB"})

    def test_reference_filter_is_reflected_in_output_bam(self):
        output = self.path / "length-filtered.bam"
        result = run(UNICORN, "refstats", "-b", self.query,
                     "--minreflen", "1500", "-o", output)
        self.assertEqual(set(table(result.stdout)), {"refB"})
        run("samtools", "quickcheck", output)
        rows = run("samtools", "view", output).stdout.splitlines()
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0].split("\t")[2], "refB")

    def test_refstats_damage_tags_preserve_alignment_and_work_in_both_orders(self):
        source = self.path / "damage-tags.sam"
        rows = [
            ("changed_forward", 0, 11, "10M", "TAAAAAAAAA", "0C9", 1),
            ("matched_forward", 0, 21, "10M", "CAAAAAAAAA", "10", 0),
            ("changed_reverse", 16, 31, "10M", "AAAAAAAAAA", "9G0", 1),
            ("with_insertion", 0, 41, "5M1I5M", "TAAAAGAAAAA", "0C9", 2),
            ("with_deletion", 0, 51, "5M1D5M", "TAAAAAAAAA", "0C4^A5", 2),
            ("with_soft_clip", 0, 61, "2S10M", "GGTAAAAAAAAA", "0C9", 1),
            ("no_diagnostic", 0, 71, "10M", "AAAAAAAAAA", "10", 0),
        ]
        with source.open("w") as fh:
            fh.write("@HD\tVN:1.6\tSO:queryname\n@SQ\tSN:refA\tLN:1000\n")
            for name, flag, pos, cigar, seq, md, nm in rows:
                fh.write(f"{name}\t{flag}\trefA\t{pos}\t60\t{cigar}\t*\t0\t0\t"
                         f"{seq}\t{'?' * len(seq)}\tNM:i:{nm}\tAS:i:7\tMD:Z:{md}\n")
            fh.write("no_md\t0\trefA\t81\t60\t10M\t*\t0\t0\t"
                     "TAAAAAAAAA\t??????????\tNM:i:1\tAS:i:7\n")
        query = self.path / "damage-tags.query.bam"
        coord = self.path / "damage-tags.coord.bam"
        run("samtools", "view", "-b", "-o", query, source)
        run("samtools", "sort", "-o", coord, query)
        for input_bam in (query, coord):
            with self.subTest(order=input_bam.name):
                output = self.path / f"{input_bam.stem}.annotated.bam"
                run(UNICORN, "refstats", "-b", input_bam, "-o", output)
                run("samtools", "quickcheck", output)
                decoded = run("samtools", "view", output).stdout.splitlines()
                self.assertEqual(len(decoded), 8)
                by_name = {r.split("\t")[0]: r.split("\t") for r in decoded}
                tags = {name: {f[:2]: f for f in fields[11:]}
                        for name, fields in by_name.items()}
                self.assertEqual(tags["changed_forward"]["AS"], "AS:i:7")
                self.assertEqual(tags["changed_reverse"]["AS"], "AS:i:7")
                self.assertAlmostEqual(float(tags["changed_forward"]["XD"].split(":")[-1]), 0.2, places=5)
                self.assertAlmostEqual(float(tags["changed_reverse"]["XD"].split(":")[-1]), 0.2, places=5)
                self.assertAlmostEqual(float(tags["with_insertion"]["XD"].split(":")[-1]), 1.2, places=5)
                self.assertAlmostEqual(float(tags["with_deletion"]["XD"].split(":")[-1]), 1.2, places=5)
                self.assertAlmostEqual(float(tags["with_soft_clip"]["XD"].split(":")[-1]),
                                       1 - 0.8 * math.exp(-2 / 5), places=5)
                self.assertAlmostEqual(float(tags["matched_forward"]["XD"].split(":")[-1]), 0.0, places=5)
                self.assertEqual(float(tags["no_diagnostic"]["XP"].split(":")[-1]), 0.0)
                self.assertGreater(float(tags["changed_forward"]["XP"].split(":")[-1]), 3)
                error = 0.001 / 3
                def mismatch_probability(damage):
                    match = ((1 - damage) * (1 - error) * (1 - 0.001)
                             + damage * error * (1 - 0.001)
                             + error * 0.001 * (1 - damage))
                    return 1 - match
                expected_pmd = math.log(mismatch_probability(0.31) /
                                        mismatch_probability(0.001))
                self.assertAlmostEqual(float(tags["changed_forward"]["XP"].split(":")[-1]),
                                       expected_pmd, places=5)
                self.assertAlmostEqual(float(tags["changed_forward"]["XP"].split(":")[-1]),
                                       float(tags["changed_reverse"]["XP"].split(":")[-1]), places=5)
                self.assertLess(float(tags["matched_forward"]["XP"].split(":")[-1]), 0)
                self.assertNotIn("XP", tags["no_md"])
                self.assertNotIn("XD", tags["no_md"])
                self.assertNotIn("XE", tags["no_md"])
                bare = self.path / f"{input_bam.stem}.bare.bam"
                run(UNICORN, "refstats", "-b", input_bam, "-o", bare, "--no-damage-tags")
                self.assertTrue(all("XP:f:" not in line and "XD:f:" not in line and "XE:f:" not in line
                                    for line in run("samtools", "view", bare).stdout.splitlines()))

    def test_experimental_score_requires_unit_penalty_as(self):
        source = self.path / "damage-score.sam"
        with source.open("w") as fh:
            fh.write("@HD\tVN:1.6\tSO:queryname\n@SQ\tSN:refA\tLN:1000\n")
            for name, as_field, prior_xe in (
                ("missing_as", "", ""),
                ("nonunit", "\tAS:i:-4", "\tXE:f:99"),
                ("unit", "\tAS:i:-1", "\tXE:f:99"),
            ):
                fh.write(f"{name}\t0\trefA\t11\t60\t10M\t*\t0\t0\t"
                         f"TAAAAAAAAA\t??????????\tNM:i:1\tMD:Z:0C9{as_field}{prior_xe}\n")
        query = self.path / "damage-score.bam"
        output = self.path / "damage-score.annotated.bam"
        run("samtools", "view", "-b", "-o", query, source)
        run(UNICORN, "refstats", "-b", query, "-o", output)
        run("samtools", "quickcheck", output)
        self.assertIn("XE=-XD-if-AS=-NM", run("samtools", "view", "-H", output).stdout)
        rows = {fields[0]: fields for line in run("samtools", "view", output).stdout.splitlines()
                if (fields := line.split("\t"))}
        self.assertEqual(set(rows), {"missing_as", "nonunit", "unit"})
        for name, fields in rows.items():
            tags = {field[:2]: field for field in fields[11:]}
            self.assertAlmostEqual(float(tags["XD"].split(":")[-1]), 0.2, places=5)
            if name == "unit":
                self.assertEqual(tags["AS"], "AS:i:-1")
                self.assertAlmostEqual(float(tags["XE"].split(":")[-1]), -0.2, places=5)
            else:
                self.assertNotIn("XE", tags)
        bare = self.path / "damage-score.no-tags.bam"
        run(UNICORN, "refstats", "-b", query, "-o", bare, "--no-damage-tags")
        bare_rows = {fields[0]: fields for line in run("samtools", "view", bare).stdout.splitlines()
                     if (fields := line.split("\t"))}
        for name in ("nonunit", "unit"):
            self.assertIn("XE:f:99", bare_rows[name])
        coord = self.path / "damage-score.coord.bam"
        coord_output = self.path / "damage-score.coord.annotated.bam"
        run("samtools", "sort", "-o", coord, query)
        run(UNICORN, "refstats", "-b", coord, "-o", coord_output)
        run("samtools", "quickcheck", coord_output)
        coord_rows = {fields[0]: fields for line in run("samtools", "view", coord_output).stdout.splitlines()
                      if (fields := line.split("\t"))}
        self.assertAlmostEqual(float(next(tag for tag in coord_rows["unit"]
                                          if tag.startswith("XE:f:")).split(":")[-1]),
                               -0.2, places=5)
        self.assertFalse(any(tag.startswith("XE:") for tag in coord_rows["nonunit"]))

    def assert_prefixed_header(self, text, identifiers, prefix, read_column):
        fields = text.splitlines()[0].split("\t")
        self.assertEqual([f.lstrip("#").split(":", 1)[0] for f in fields],
                         [str(i) for i in range(1, len(fields) + 1)])
        names = [f.split(":", 1)[1] for f in fields]
        self.assertEqual(names[:len(identifiers)], identifiers)
        self.assertTrue(all(n.startswith(prefix) for n in names[len(identifiers):]))
        self.assertIn(read_column, names)
        self.assertEqual(len(names), len(set(names)))
        for row in text.splitlines()[1:]:
            self.assertEqual(len(row.split("\t")), len(fields))

    def test_refstats_metric_headers_are_prefixed(self):
        for source in [self.query, self.coord]:
            for taxonomy in [False, True]:
                with self.subTest(source=source.name, taxonomy=taxonomy):
                    args = [UNICORN, "refstats", "-b", source]
                    if taxonomy:
                        args += ["--names", self.names, "--nodes", self.nodes,
                                 "--acc2tax", self.acc2tax]
                    result = run(*args)
                    self.assert_prefixed_header(result.stdout,
                                                ["Id", "taxID"] if taxonomy else ["Id"],
                                                "rs_", "rs_nreads")

    def test_taxstats_metric_headers_are_prefixed(self):
        result = run(UNICORN, "taxstats", "-b", self.query,
                     "--names", self.names, "--nodes", self.nodes, "--rank", "genus",
                     "--acc2tax", self.acc2tax)
        self.assert_prefixed_header(result.stdout, ["taxid", "name"], "ts_", "ts_nreads")
        self.assertIn("ts_num_accessions", result.stdout.splitlines()[0])
        self.assertIn("ts_total_length", result.stdout.splitlines()[0])
        self.assertEqual(set(table(result.stdout)), {"2"})

    def test_taxstats_counts_exact_colliding_names_in_both_paths(self):
        xr = self.path / "xr-sorted.bam"
        run("samtools", "sort", "-t", "XR", "-o", xr, self.query)
        for source in [self.query, self.coord, xr]:
            with self.subTest(order=source.name):
                result = run(UNICORN, "taxstats", "-b", source,
                             "--names", self.names, "--nodes", self.nodes,
                             "--acc2tax", self.acc2tax, "--rank", "genus",
                             "--minreads", "2")
                self.assert_prefixed_header(result.stdout, ["taxid", "name"], "ts_", "ts_nreads")
                columns = [x.split(":",1)[1] for x in result.stdout.splitlines()[0].split("\t")]
                rows = table(result.stdout)
                self.assertEqual(set(rows), {"2"})
                self.assertEqual(int(rows["2"][columns.index("ts_nreads")]), 2)
                self.assertEqual(int(rows["2"][columns.index("ts_num_alns")]), 4)
                self.assertIn("2 reads, 2 passed filters", result.stderr)

    def test_taxstats_minreads_respected_for_xr_sorted_input(self):
        xr = self.path / "xr-minreads.bam"
        run("samtools", "sort", "-t", "XR", "-o", xr, self.query)
        for source in [self.query, self.coord, xr]:
            result = run(UNICORN, "taxstats", "-b", source,
                         "--names", self.names, "--nodes", self.nodes,
                         "--acc2tax", self.acc2tax, "--rank", "genus", "--minreads", "3")
            self.assertEqual(len(result.stdout.strip().splitlines()), 1)

    def test_taxstats_mean_precision_on_large_group(self):
        source = self.path / "precision.sam"
        seq = "ACGT" * 8
        count = 100000
        with source.open("w") as f:
            f.write("@HD\tVN:1.6\tSO:queryname\n@SQ\tSN:refA\tLN:1000\n")
            for i in range(count):
                nm = 1 if i < count // 2 else 0
                f.write(f"p{i:06d}\t0\trefA\t11\t60\t32M\t*\t0\t0\t{seq}\t{'I'*32}\tNM:i:{nm}\tAS:i:0\n")
        bam = self.path / "precision.bam"
        run("samtools", "view", "-b", "-o", bam, source)
        result = run(UNICORN, "taxstats", "-b", bam,
                     "--names", self.names, "--nodes", self.nodes,
                     "--acc2tax", self.acc2tax, "--rank", "genus")
        columns = [x.split(":",1)[1] for x in result.stdout.splitlines()[0].split("\t")]
        row = table(result.stdout)["2"]
        self.assertEqual(int(row[columns.index("ts_nreads")]), count)
        self.assertAlmostEqual(float(row[columns.index("ts_mean_alnnm")]), 0.5, places=6)
        self.assertAlmostEqual(float(row[columns.index("ts_mean_alnani")]), 98.4375, places=5)

    def test_taxstats_two_read_sample_standard_deviation(self):
        source = self.path / "two-read-lengths.sam"
        with source.open("w") as f:
            f.write("@HD\tVN:1.6\tSO:queryname\n@SQ\tSN:refA\tLN:1000\n")
            for i,length in enumerate([32,64]):
                seq = "ACGT" * (length//4)
                f.write(f"length{i}\t0\trefA\t11\t60\t{length}M\t*\t0\t0\t{seq}\t{'I'*length}\tNM:i:0\tAS:i:0\tXR:i:2\n")
        bam = self.path / "two-read-lengths.bam"
        xr = self.path / "two-read-lengths.xr.bam"
        run("samtools", "view", "-b", "-o", bam, source)
        run("samtools", "sort", "-t", "XR", "-o", xr, bam)
        for input_bam in [bam,xr]:
            result = run(UNICORN, "taxstats", "-b", input_bam,
                         "--names", self.names, "--nodes", self.nodes,
                         "--acc2tax", self.acc2tax, "--rank", "genus")
            columns = [x.split(":",1)[1] for x in result.stdout.splitlines()[0].split("\t")]
            row = table(result.stdout)["2"]
            self.assertAlmostEqual(float(row[columns.index("ts_mean_readl")]), 48.0, places=6)
            self.assertAlmostEqual(float(row[columns.index("ts_stdev_readl")]), 512**0.5, places=6)

    def test_refstats_counts_distinct_colliding_names(self):
        for source in [self.query, self.coord]:
            result=run(UNICORN,"refstats","-b",source)
            columns=[x.split(":",1)[1] for x in result.stdout.splitlines()[0].split("\t")]
            self.assertEqual(int(table(result.stdout)["refA"][columns.index("rs_nreads")]),2)

    def test_bamstats_counts_exact_names_despite_hash_collision(self):
        for source in [self.query, self.coord]:
            with self.subTest(order=source.name):
                rows = list(table(run(UNICORN, "bamstats", "-b", source).stdout).values())
                self.assertEqual(int(rows[0][1]), 4)
                self.assertEqual(int(rows[0][2]), 2)


if __name__ == "__main__":
    unittest.main()
