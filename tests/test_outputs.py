"""CLI regressions; requires a built unicorn, Python 3, and samtools on PATH."""
import os
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
