"""Exact multi-rank taxstats must agree with three independent BAM passes."""

import csv
import gzip
import math
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_outputs import UNICORN


RANKS = ("family", "genus", "species")
INTEGER_COLUMNS = {
    "ts_num_accessions", "ts_total_length", "ts_num_alns", "ts_nreads",
    "ts_mode_readl", "ts_readl_min", "ts_readl_max", "ts_num_covbases",
    "ts_spatial_nalns", "ts_spatial_invalid", "ts_ref_spatial_nvalid",
}


def run(*args):
    result = subprocess.run([str(arg) for arg in args], capture_output=True,
                            text=True, timeout=120)
    if result.returncode:
        raise AssertionError(f"Command {args} returned {result.returncode}:\n{result.stderr}")
    return result


def tax_table(path):
    with gzip.open(path, "rt", newline="") as fp:
        columns = [item.split(":", 1)[-1] for item in next(fp).rstrip("\n").split("\t")]
        rows = list(csv.DictReader(fp, fieldnames=columns, delimiter="\t"))
    return columns, {row["taxid"]: row for row in rows}


def length_table(path):
    with gzip.open(path, "rt", newline="") as fp:
        return {(row["taxid"], row["length"]): int(row["count"])
                for row in csv.DictReader(fp, delimiter="\t")}


class ExactMultiRank(unittest.TestCase):
    def test_rejects_invalid_rank_lists_and_option_conflicts(self):
        with tempfile.TemporaryDirectory(prefix="unicorn-multirank-opts-") as td:
            p = Path(td)
            base = [UNICORN, "taxstats", "--acc2tax", p / "map.tsv",
                    "-b", p / "missing.bam"]
            cases = (
                ("no output", ["--ranks", "family,genus"]),
                ("explicit rank", ["--ranks", "family,genus", "--rank", "species",
                                   "--outstat", p / "out.tsv"]),
                ("length path", ["--ranks", "family,genus", "--read-lengths",
                                  p / "lengths.tsv.gz", "--outstat", p / "out.tsv"]),
                ("duplicate", ["--ranks", "family,genus,genus",
                               "--outstat", p / "out.tsv"]),
                ("empty item", ["--ranks", "family,,genus",
                                "--outstat", p / "out.tsv"]),
                ("too many", ["--ranks", "family,genus,species,order",
                              "--outstat", p / "out.tsv"]),
                ("assigned", ["--ranks", "assigned", "--outstat", p / "out.tsv"]),
            )
            for label, args in cases:
                with self.subTest(case=label):
                    result = subprocess.run([str(arg) for arg in (*base, *args)],
                                            capture_output=True, text=True, timeout=10)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("--ranks", result.stderr)

    def test_matches_three_direct_runs(self):
        with tempfile.TemporaryDirectory(prefix="unicorn-multirank-") as td:
            p = Path(td)
            names = p / "names.dmp"
            nodes = p / "nodes.dmp"
            acc = p / "accessions.tsv"
            taxonomy = (
                (1, 1, "no rank", "root"),
                (10, 1, "family", "family A"),
                (11, 1, "family", "family B"),
                (12, 1, "family", "family C"),
                (20, 10, "genus", "genus A"),
                (21, 11, "genus", "genus B"),
                (22, 11, "genus", "genus C"),
                (30, 20, "species", "species A"),
                (31, 20, "species", "species B"),
                (32, 21, "species", "species C"),
            )
            nodes.write_text("".join(f"{taxid}\t|\t{parent}\t|\t{rank}\t|\n"
                                     for taxid, parent, rank, _ in taxonomy))
            names.write_text("".join(f"{taxid}\t|\t{name}\t|\t\t|\tscientific name\t|\n"
                                     for taxid, _, _, name in taxonomy))
            refs = {"refA": (400, 30, 20), "refB": (500, 31, 20),
                    "refC": (800, 32, 21), "refD": (1000, 22, 22),
                    "refE": (1100, 12, 12)}
            acc.write_text("accession\taccession.version\ttaxid\tgi\n" +
                           "".join(f"{ref}\t{ref}\t{taxid}\t0\n"
                                   for ref, (_, taxid, _) in refs.items()))

            # Two names collide under the old 32-bit read-name hash. Reads
            # shared across species and genera test rank-specific deduplication.
            collision_a = "read79b3d9f3f6a4854a"
            collision_b = "reade1b024b95172fb60"
            alignments = (
                ("shared_species", "refA", 1, 50, 0, 0),
                ("shared_species", "refB", 11, 50, 2, 256),
                ("shared_families", "refA", 80, 40, 1, 0),
                ("shared_families", "refC", 15, 40, 3, 256),
                (collision_a, "refA", 160, 36, 0, 0),
                (collision_b, "refB", 95, 31, 1, 0),
                ("unique_c", "refC", 100, 45, 1, 0),
                ("shared_genus", "refC", 200, 38, 1, 0),
                ("shared_genus", "refD", 140, 38, 0, 256),
                ("unique_d", "refD", 27, 48, 0, 0),
                ("family_only", "refE", 99, 42, 2, 0),
            )
            sam = p / "input.sam"
            with sam.open("w") as fp:
                fp.write("@HD\tVN:1.6\tSO:unsorted\n")
                for ref, (length, _, _) in refs.items():
                    fp.write(f"@SQ\tSN:{ref}\tLN:{length}\n")
                for name, ref, pos, length, nm, flag in alignments:
                    seq = ("ACGT" * ((length + 3) // 4))[:length]
                    fp.write(f"{name}\t{flag}\t{ref}\t{pos}\t60\t{length}M\t*\t0\t0\t"
                             f"{seq}\t{'I' * length}\tNM:i:{nm}\tXR:i:{refs[ref][2]}\n")
            coordinate = p / "coordinate.bam"
            xr_sorted = p / "xr-genus.bam"
            run("samtools", "sort", "-o", coordinate, sam)
            run("samtools", "sort", "-t", "XR", "-o", xr_sorted, sam)
            base = [UNICORN, "taxstats", "--acc2tax", acc,
                    "--names", names, "--nodes", nodes]

            mislabeled = p / "mislabeled-coordinate.sam"
            mislabeled.write_text(sam.read_text().replace("SO:unsorted", "SO:coordinate", 1))
            bad = subprocess.run(
                [str(arg) for arg in (*base, "-b", mislabeled, "--ranks",
                                      ",".join(RANKS), "--outstat", p / "bad.tsv")],
                capture_output=True, text=True, timeout=120)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("reference IDs decrease", bad.stderr)

            cases = (("coord", coordinate, 1, ()),
                     ("xr", xr_sorted, 1, ()),
                     ("threshold", coordinate, 3, ()),
                     ("spill", coordinate, 1, ("--memory-limit", "1M")))
            for label, source, minreads, extra in cases:
                with self.subTest(source=label):
                    stem = p / f"combined-{label}.tsv"
                    run(*base, "-b", source, "--ranks", ",".join(RANKS),
                        "--minreads", minreads, *extra, "--outstat", stem)
                    for rank in RANKS:
                        # Direct runs use coordinate order because the XR tags
                        # in the second fixture encode genus, not this rank.
                        direct_stem = p / f"direct-{label}-{rank}.tsv"
                        run(*base, "-b", coordinate, "--rank", rank,
                            "--minreads", minreads, "--outstat", direct_stem)
                        multi_path = p / f"combined-{label}.{rank}.tsv.gz"
                        direct_path = p / f"direct-{label}-{rank}.tsv.gz"
                        with multi_path.open("rb") as fp:
                            self.assertEqual(fp.read(4), b"\x1f\x8b\x08\x04")
                        columns, multi = tax_table(multi_path)
                        direct_columns, single = tax_table(direct_path)
                        self.assertEqual(columns, direct_columns)
                        self.assertEqual(set(multi), set(single))
                        for taxid in single:
                            for column in columns:
                                observed, expected = multi[taxid][column], single[taxid][column]
                                if column in ("taxid", "name") or column in INTEGER_COLUMNS:
                                    self.assertEqual(observed, expected, (label, rank, taxid, column))
                                else:
                                    x, y = float(observed), float(expected)
                                    self.assertTrue((math.isnan(x) and math.isnan(y)) or
                                                    math.isclose(x, y, rel_tol=2e-5, abs_tol=2e-5),
                                                    (label, rank, taxid, column, x, y))
                        multi_lengths = p / f"combined-{label}.{rank}.readlengths.tsv.gz"
                        direct_lengths = p / f"direct-{label}-{rank}.readlengths.tsv.gz"
                        self.assertEqual(length_table(multi_lengths), length_table(direct_lengths))
                        if label == "coord" and rank == "genus":
                            self.assertEqual(int(multi["20"]["ts_nreads"]), 4)
                        if label == "coord" and rank == "family":
                            self.assertEqual(int(multi["11"]["ts_nreads"]), 4)
                        if label == "coord" and rank == "species":
                            self.assertIn("22", multi)  # assigned at genus
                            self.assertIn("12", multi)  # assigned at family
                        if label == "threshold" and rank == "genus":
                            self.assertIn("20", multi)  # species 31 has only two reads
                            self.assertNotIn("22", multi)


if __name__ == "__main__":
    unittest.main()
