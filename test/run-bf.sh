
conda activate bam-filter # you need this, see bamfilter github for instructions

bam=test/data/test.bam
base=$(basename $bam .bam)

filterBAM reassign --threads 10 --bam $bam --tmp-dir tmp/ --out-bam test/data/$base.reassign.bam --iters 0 --min-read-ani 94 --min-read-count 3
filterBAM filter --threads 10 --bam test/data/$base.reassign.bam --tmp-dir tmp/ --bam-filtered test/data/$base.filter.bam --stats test/$base.stats.tsv --stats-filtered test/$base.stats_filtered.tsv --min-read-ani 94 --min-read-count 3 --min-expected-breadth-ratio 0.5 --min-normalized-entropy auto --min-normalized-gini auto --min-breadth 0 --min-avg-read-ani 90 --min-coverage-evenness 0.4 --min-coverage-mean 0 --include-low-detection
