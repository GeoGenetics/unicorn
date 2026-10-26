# Known-origin filter pilot

Requires Python 3 (NumPy for tuning), Bowtie2 and samtools. Run from the repository:

```sh
export UNICORN_BENCHMARK_DIR=/absolute/path/to/new/benchmark-directory
export UNICORN=/absolute/path/to/unicorn
export BOWTIE=/absolute/path/to/bowtie2
export SAMTOOLS=/absolute/path/to/samtools
python tools/filter_benchmark/fetch_refs.py
python tools/filter_benchmark/run.py
python tools/filter_benchmark/tune.py
```

The downloader retrieves eight public NCBI chloroplast records, including six Salix
species and Betula/Pinus background. The accession list is fixed for reproducibility.
Reference identities, sequences, hashes, commands and logs are retained. Inspect
reference_metadata.json and GenBank records before using a modified accession list.

The runner generates 72 mixtures: six source species x four scenarios x three
realizations spanning terminal damage probabilities 0, 0.05 and 0.15. Scenarios:
one abundant Salix, abundant plus rare close relative, source excluded from the
mapping database, and no Salix. Counts are 600 abundant, 20 rare and 100 each of
Betula/Pinus. Reads are 30-80 nt, uniform genomic starts, 0.5% substitutions, and
10% resampling of existing sequences with new names. These are simplified ancient-
like simulations, not fitted damage models or independent replicates per damage
condition. Circular-origin-spanning reads may be lost on the linear mapping index.

Bowtie2 uses -k 1000 with permissive score parameters recorded in run.py. It maps
against eight chloroplasts, not the original BAM's full database. ALLTOP is applied
at ANI >=93,97,99,100 and <=100; refstats applies DUST <=20 or <=100. Every alignment
filter configuration recomputes the downstream metrics. Named tables may be
compressed for archival; intermediate stdout tables are plain TSV.

Threshold tuning uses conjunctions of minimum read count, entropy bounds, minimum
k-mer-diversity duplicity, and maximum raw or sqrt(alignment_count)-scaled KS.
A fixed grid is refined with 30,000 deterministic random proposals using training
quantiles only. Training sources: caprea, cupularis, reticulata, elaeagnos. Held-out
sources: koriyanagi and viminalis. Rare pairs stay within the respective partition.
Background species are shared, and reference sequences for held-out sources remain
in the database; this is source-mixture holdout, not database novelty validation.

The objective is maximum training recall subject to >=95% observed precision.
If no rule meets it, the output explicitly reports failure and labels the best
precision rule exploratory. Thresholds are never automatically applied to Unicorn.
Species metrics count all six Salix candidates and all truly present Salix taxa,
including the missing-database source. Genus evaluation is ANY accepted Salix
reference, not rerunning taxstats or summing its read counts. Ablations project the
same proposal set with one component disabled and retune on training sources.
Held-out data must not be used to choose among the resulting variants.

Existing files are cached to permit interruption recovery. Use a fresh output
folder after changing software, mapper parameters, simulation or references;
cached outputs are not automatically invalidated. Small benchmark results cannot
establish a 95% real-world precision guarantee. Further validation needs nuclear
references, real mock/negative samples, multiple samples per damage/abundance
condition, and independent clades with realistic database competition.
