import csv
import gzip
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_outputs import UNICORN, run


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "taxstats_all_ranks.py"


def rows(path):
    opener = gzip.open if str(path).endswith(".gz") else open
    with opener(path, "rt") as fp:
        first = fp.readline().rstrip("\n")
        names = [field.split(":", 1)[-1] for field in first.split("\t")]
        return list(csv.DictReader(fp, fieldnames=names, delimiter="\t"))


class AllRankRollup(unittest.TestCase):
    def test_assigned_taxids_and_ancestor_associations(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td)
            names = p / "names.dmp"
            nodes = p / "nodes.dmp"
            acc = p / "accessions.tsv"
            names.write_text("".join(f"{t}\t|\t{name}\t|\t\t|\tscientific name\t|\n" for t, name in
                                     [(1, "root"), (2, "genus"), (3, "species A"), (4, "species B")]))
            nodes.write_text("1\t|\t1\t|\tno rank\t|\n2\t|\t1\t|\tgenus\t|\n"
                             "3\t|\t2\t|\tspecies\t|\n4\t|\t2\t|\tspecies\t|\n")
            acc.write_text("accession\taccession.version\ttaxid\tgi\nA\tA\t3\t0\nB\tB\t4\t0\n")
            sam = p / "input.sam"
            with sam.open("w") as fp:
                fp.write("@HD\tVN:1.6\tSO:unsorted\n@SQ\tSN:A\tLN:100\n@SQ\tSN:B\tLN:100\n")
                for name, ref, pos in (("q1", "A", 1), ("q2", "A", 51),
                                       ("q1", "B", 1), ("q3", "B", 1)):
                    fp.write(f"{name}\t0\t{ref}\t{pos}\t60\t50M\t*\t0\t0\t{('ACGT'*13)[:50]}\t{'I'*50}\tNM:i:0\tXR:i:2\n")
            bam = p / "xr.bam"
            run("samtools", "sort", "-t", "XR", "-o", bam, sam)
            base = [UNICORN, "taxstats", "-b", bam, "--names", names, "--nodes", nodes, "--acc2tax", acc]
            missing_map = subprocess.run([str(UNICORN), "taxstats", "-b", str(bam),
                                          "--names", str(names), "--nodes", str(nodes),
                                          "--rank", "assigned"], capture_output=True, text=True)
            self.assertNotEqual(missing_map.returncode, 0)
            self.assertIn("--rank assigned requires --acc2tax", missing_map.stderr)
            assigned = p / "assigned.tsv"
            lengths = p / "assigned.readlengths.tsv.gz"
            result = run(*base, "--rank", "assigned", "--outstat", assigned, "--read-lengths", lengths)
            self.assertEqual(result.returncode, 0)
            leaf = {int(r["taxid"]): r for r in rows(assigned)}
            self.assertEqual(set(leaf), {3, 4})
            self.assertEqual([int(leaf[t]["ts_nreads"]) for t in (3, 4)], [2, 2])
            exact = {int(r["taxid"]): r for r in rows_from_text(run(*base, "--rank", "genus").stdout)}
            self.assertEqual(int(exact[2]["ts_nreads"]), 3)

            output = p / "all.tsv.gz"
            hist = p / "all.readlengths.tsv.gz"
            subprocess.run([sys.executable, SCRIPT, "--taxstats", assigned,
                            "--nodes", nodes, "--names", names, "--output", output,
                            "--starting-level", "assigned_taxid", "--read-lengths", lengths,
                            "--out-read-lengths", hist], check=True)
            all_rows = {int(r["taxid"]): r for r in rows(output)}
            self.assertEqual(set(all_rows), {1, 2, 3, 4})
            self.assertEqual(int(all_rows[2]["ts_nread_associations"]), 4)
            self.assertEqual(int(all_rows[2]["ts_alignment_associations"]), 4)
            self.assertEqual(int(all_rows[2]["ts_reference_associations"]), 2)
            self.assertEqual(int(all_rows[2]["n_starting_rows"]), 2)
            self.assertEqual(float(all_rows[2]["ts_pooled_breadth_associations"]), .75)
            self.assertAlmostEqual(float(all_rows[2]["ts_nread_weighted_leaf_ref_mean_breadth"]), .75)
            self.assertEqual(float(all_rows[2]["ts_readlength_median_associations"]), 50)
            self.assertEqual(sum(int(r["count"]) for r in rows(hist) if int(r["taxid"]) == 2), 4)
            self.assertEqual(all_rows[1]["ts_nread_associations"], all_rows[2]["ts_nread_associations"])
            with output.open("rb") as fp:
                self.assertEqual(fp.read(4), b"\x1f\x8b\x08\x04")  # BGZF gzip extra field

    def test_missing_taxonomy_fails(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td)
            (p / "nodes").write_text("1\t|\t1\t|\tno rank\t|\n")
            (p / "names").write_text("1\t|\troot\t|\t\t|\tscientific name\t|\n")
            (p / "taxa").write_text("#1:taxid\t2:ts_num_accessions\t3:ts_total_length\t4:ts_num_alns\t5:ts_nreads\t6:ts_num_covbases\n"
                                    "99\t1\t100\t1\t1\t50\n")
            r = subprocess.run([sys.executable, SCRIPT, "--taxstats", p / "taxa", "--nodes", p / "nodes",
                                "--names", p / "names", "--output", p / "result", "--starting-level",
                                "assigned_taxid", "--plain-text", "--unknown-policy", "error"], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("missing or cyclic taxonomy", r.stderr)
            run_result = subprocess.run([sys.executable, SCRIPT, "--taxstats", p / "taxa", "--nodes", p / "nodes",
                                         "--names", p / "names", "--output", p / "result", "--starting-level",
                                         "assigned_taxid", "--plain-text"], capture_output=True, text=True)
            self.assertEqual(run_result.returncode, 0, run_result.stderr)
            unresolved = rows(p / "result")[0]
            self.assertEqual(unresolved["lineage_status"], "unresolved")
            self.assertEqual(unresolved["ts_nread_associations"], "1")


def rows_from_text(text):
    lines = text.splitlines()
    fields = [f.split(":", 1)[-1] for f in lines[0].split("\t")]
    return list(csv.DictReader(lines[1:], fieldnames=fields, delimiter="\t"))


if __name__ == "__main__":
    unittest.main()
