#!/usr/bin/env python3
"""Roll a taxstats table from its starting taxids to every observed ancestor.

Counts are starting-taxon associations, not distinct reads at ancestors. This
postprocessor reads the taxstats table once and never rescans the BAM.
"""

import argparse
import csv
import gzip
import math
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile


BASE_METRICS = (
    "ts_mean_readl", "ts_mean_alnnm", "ts_mean_alnani", "ts_mdust",
    "ts_breath_cov", "ts_breath_ratio", "ts_mean_cov", "ts_mean_covcovered",
    "ts_evenness_cov", "ts_duplicity",
)
COUNTS = ("ts_num_accessions", "ts_total_length", "ts_num_alns", "ts_nreads", "ts_num_covbases")


def open_text(path):
    return gzip.open(path, "rt", newline="") if str(path).endswith(".gz") else open(path, newline="")


def bgzf_writer(path, plain=False):
    """Return a text stream and a finalizer; default named output is BGZF."""
    if plain:
        fp = open(path, "w", newline="")
        return fp, lambda: fp.close()
    bgzip = shutil.which("bgzip")
    if bgzip is None:
        raise RuntimeError("bgzip is required for BGZF output; install htslib or use --plain-text")
    raw = open(path, "wb")
    proc = subprocess.Popen([bgzip, "-c"], stdin=subprocess.PIPE, stdout=raw)
    import io
    fp = io.TextIOWrapper(proc.stdin, encoding="utf-8", newline="")

    def finish():
        try:
            fp.close()
            if proc.wait() != 0:
                raise RuntimeError("bgzip failed")
        finally:
            raw.close()

    return fp, finish


def load_nodes(path):
    nodes = {}
    with open_text(path) as fp:
        for line in fp:
            fields = line.split("|", 3)
            if len(fields) >= 3:
                nodes[int(fields[0])] = (int(fields[1]), fields[2].strip())
    return nodes


def ancestors(taxid, nodes, cache):
    if taxid in cache:
        return cache[taxid]
    path, seen = [], set()
    while taxid not in cache:
        if taxid in seen or taxid not in nodes:
            raise ValueError(f"missing or cyclic taxonomy lineage at taxid {taxid}")
        seen.add(taxid)
        path.append(taxid)
        parent = nodes[taxid][0]
        if parent == taxid:
            cache[taxid] = (taxid,)
            break
        taxid = parent
    for t in reversed(path):
        if t not in cache:
            cache[t] = (t,) + cache[nodes[t][0]]
    return cache[path[0]]


def lineage(taxid, nodes, cache, unresolved, policy):
    if taxid in unresolved:
        return (taxid,)
    try:
        return ancestors(taxid, nodes, cache)
    except ValueError:
        if policy == "error":
            raise
        unresolved.add(taxid)
        return (taxid,)


def selected_names(path, selected):
    names = {}
    with open_text(path) as fp:
        for line in fp:
            fields = line.split("|", 4)
            if len(fields) >= 4 and fields[3].strip() == "scientific name":
                taxid = int(fields[0])
                if taxid in selected:
                    names[taxid] = fields[1].strip()
    return names


def rollup(args):
    nodes = load_nodes(args.nodes)
    cache, unresolved, unknown_names = {}, set(), {}
    with open_text(args.taxstats) as fp:
        first = fp.readline().rstrip("\r\n")
        columns = [field.split(":", 1)[-1] for field in first.split("\t")]
        required = {"taxid", *COUNTS}
        if not required.issubset(columns):
            raise ValueError(f"taxstats missing required columns: {sorted(required - set(columns))}")
        metrics = [m for m in BASE_METRICS if m in columns]
        metrics += [m for m in columns if m.startswith("ts_ref_mean_")]
        # Each node holds counts, followed by metric-specific weighted sum and
        # denominator. Keep denominators so a nonfinite metric omits only itself.
        width = len(COUNTS) + 1 + 2 * len(metrics)
        aggregates = {}
        for row in csv.DictReader(fp, fieldnames=columns, delimiter="\t"):
            taxid = int(row["taxid"])
            path = lineage(taxid, nodes, cache, unresolved, args.unknown_policy)
            if taxid in unresolved:
                unknown_names[taxid] = row.get("name") or "unresolved"
            counts = [int(row[c]) for c in COUNTS]
            weight = counts[COUNTS.index("ts_nreads")]
            local = counts + [1] + [0.0] * (2 * len(metrics))
            for i, name in enumerate(metrics):
                value = float(row[name])
                if math.isfinite(value):
                    local[len(COUNTS) + 1 + 2 * i] = weight * value
                    local[len(COUNTS) + 2 + 2 * i] = weight
            for ancestor in path:
                current = aggregates.setdefault(ancestor, [0.0] * width)
                for i, value in enumerate(local):
                    current[i] += value

    names = selected_names(args.names, aggregates)
    names.update(unknown_names)
    if len(names) != len(aggregates):
        raise ValueError("scientific names missing for one or more observed taxonomy nodes")

    median = {}
    if args.read_lengths:
        if not args.out_read_lengths:
            raise ValueError("--out-read-lengths is required with --read-lengths")
        with tempfile.TemporaryDirectory(prefix="unicorn-rollup-", dir=args.tmpdir) as td:
            db = sqlite3.connect(str(Path(td) / "lengths.sqlite3"))
            db.execute("CREATE TABLE h (taxid INTEGER, length INTEGER, count INTEGER, PRIMARY KEY(taxid,length)) WITHOUT ROWID")
            with open_text(args.read_lengths) as fp:
                reader = csv.DictReader(fp, delimiter="\t")
                if not {"taxid", "length", "count"}.issubset(reader.fieldnames or ()):
                    raise ValueError("read-length table must contain taxid, length, count")
                for row in reader:
                    taxid, length, count = (int(row[c]) for c in ("taxid", "length", "count"))
                    if count < 0:
                        raise ValueError("negative read-length count")
                    for ancestor in lineage(taxid, nodes, cache, unresolved, args.unknown_policy):
                        db.execute("INSERT INTO h VALUES (?,?,?) ON CONFLICT(taxid,length) DO UPDATE SET count=count+excluded.count", (ancestor, length, count))
            db.commit()
            output, finish = bgzf_writer(args.out_read_lengths, args.plain_text)
            try:
                writer = csv.writer(output, delimiter="\t", lineterminator="\n")
                writer.writerow(("taxid", "length", "count"))
                current_taxid, seen, middle = None, 0, []
                for taxid, length, count in db.execute("SELECT taxid,length,count FROM h ORDER BY taxid,length"):
                    if taxid not in aggregates:
                        raise ValueError(f"histogram taxid {taxid} absent from taxstats")
                    if current_taxid != taxid:
                        if current_taxid is not None:
                            expected = int(aggregates[current_taxid][COUNTS.index("ts_nreads")])
                            if seen != expected:
                                raise ValueError(f"histogram count differs from ts_nreads at {current_taxid}")
                            median[current_taxid] = sum(middle) / 2
                        current_taxid, seen, middle = taxid, 0, []
                    total = int(aggregates[taxid][COUNTS.index("ts_nreads")])
                    if seen <= (total - 1) // 2 < seen + count:
                        middle.append(length)
                    if seen <= total // 2 < seen + count:
                        middle.append(length)
                    seen += count
                    if total >= args.min_associations:
                        writer.writerow((taxid, length, count))
                if current_taxid is not None and seen != int(aggregates[current_taxid][COUNTS.index("ts_nreads")]):
                    raise ValueError(f"histogram count differs from ts_nreads at {current_taxid}")
                if current_taxid is not None:
                    median[current_taxid] = sum(middle) / 2
                if len(median) != len(aggregates):
                    raise ValueError("read-length histogram is missing one or more starting taxids")
            finally:
                finish()
                db.close()

    fields = ("taxid", "parent_taxid", "rank", "name", "lineage_status", "starting_level", "n_starting_rows",
              "ts_nread_associations", "ts_alignment_associations", "ts_reference_associations",
              "ts_reference_length_associations", "ts_covered_base_associations",
              "ts_pooled_breadth_associations", "ts_readlength_median_associations")
    fields += tuple("ts_nread_weighted_leaf_" + m[3:] for m in metrics)
    output, finish = bgzf_writer(args.output, args.plain_text)
    try:
        writer = csv.writer(output, delimiter="\t", lineterminator="\n")
        writer.writerow(fields)
        for taxid in sorted(aggregates):
            values = aggregates[taxid]
            counts = [int(v) for v in values[:len(COUNTS)]]
            nrefs, length, nalns, nreads, covered = counts
            if nreads < args.min_associations:
                continue
            weighted = []
            for i in range(len(metrics)):
                idx = len(COUNTS) + 1 + 2 * i
                weighted.append(values[idx] / values[idx + 1] if values[idx + 1] else "nan")
            parent, rank = ("", "unresolved") if taxid in unresolved else nodes[taxid]
            writer.writerow((taxid, parent, rank, names[taxid],
                             "unresolved" if taxid in unresolved else "known",
                             args.starting_level, int(values[len(COUNTS)]), nreads, nalns,
                             nrefs, length, covered, covered / length if length else "nan",
                             median.get(taxid, "nan"), *weighted))
    finally:
        finish()
    return len(aggregates), len(unresolved), sum(int(aggregates[t][COUNTS.index("ts_nreads")]) for t in unresolved)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--taxstats", required=True, type=Path)
    parser.add_argument("--nodes", required=True, type=Path)
    parser.add_argument("--names", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--starting-level", required=True, help="e.g. assigned_taxid or genus; written to every row")
    parser.add_argument("--min-associations", type=int, default=1, help="reporting threshold after aggregation")
    parser.add_argument("--unknown-policy", choices=("retain", "error"), default="retain",
                        help="retain unresolved taxids as unpropagated rows, or fail (default: retain)")
    parser.add_argument("--read-lengths", type=Path)
    parser.add_argument("--out-read-lengths", type=Path)
    parser.add_argument("--tmpdir", type=Path, help="disk spill directory for optional read-length rollup")
    parser.add_argument("--plain-text", action="store_true", help="write ordinary TSV instead of BGZF")
    args = parser.parse_args()
    if args.min_associations < 1:
        parser.error("--min-associations must be positive")
    if args.read_lengths and not args.out_read_lengths:
        parser.error("--out-read-lengths is required with --read-lengths")
    if not args.plain_text and any(not str(path).endswith(".gz") for path in
                                   (args.output, args.out_read_lengths) if path):
        parser.error("BGZF output paths must end with .gz")
    inputs = {args.taxstats.resolve(), args.nodes.resolve(), args.names.resolve()}
    if args.read_lengths:
        inputs.add(args.read_lengths.resolve())
    outputs = {args.output.resolve()}
    if args.out_read_lengths:
        outputs.add(args.out_read_lengths.resolve())
    if inputs & outputs or len(outputs) != (2 if args.out_read_lengths else 1):
        parser.error("output paths must be distinct from each other and from input paths")
    try:
        n, unknown, unknown_reads = rollup(args)
    except (OSError, ValueError, RuntimeError, sqlite3.Error) as exc:
        parser.exit(1, f"taxstats_all_ranks: {exc}\n")
    print(f"taxstats_all_ranks: wrote {n} observed nodes, including {unknown} unresolved taxids"
          f" with {unknown_reads} read associations excluded from ancestor totals", file=sys.stderr)


if __name__ == "__main__":
    main()
