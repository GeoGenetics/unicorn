"""Independent coverage oracle for taxstats' compact reference state."""
import math
import random
import tempfile
import unittest
from pathlib import Path
from test_outputs import run, UNICORN

class TaxstatsCoverage(unittest.TestCase):
    def test_coverage_moments_across_references_and_sort_orders(self):
        rng = random.Random(7401)
        with tempfile.TemporaryDirectory() as td:
            d = Path(td)
            # Include short references, gaps, identical intervals, touching ends,
            # long references and multiple genera. No dense array for long refs.
            lengths = [80, 500, 100000000, 1000]
            taxa = [2, 2, 3, 3]
            intervals = [[] for _ in lengths]
            records = []
            for ref, length in enumerate(lengths):
                for j in range(70):
                    start = rng.randrange(0, min(length - 32, 800))
                    end = start + 32
                    intervals[ref].append((start, end))
                    seq = 'ACGT' * 8
                    records.append(f'q{ref}_{j}\t0\tr{ref}\t{start+1}\t60\t32M\t*\t0\t0\t{seq}\t'+('I'*32)+f'\tNM:i:0\tXR:i:{taxa[ref]}\n')
            rng.shuffle(records)
            sam = d/'input.sam'
            sam.write_text('@HD\tVN:1.6\tSO:unsorted\n'+''.join(f'@SQ\tSN:r{i}\tLN:{l}\n' for i,l in enumerate(lengths))+''.join(records))
            names=d/'names';nodes=d/'nodes';acc=d/'acc'
            names.write_text(''.join(f'{i}\t|\tname{i}\t|\t\t|\tscientific name\t|\n' for i in [1,2,3]))
            nodes.write_text('1\t|\t1\t|\tno rank\t|\n2\t|\t1\t|\tgenus\t|\n3\t|\t1\t|\tgenus\t|\n')
            acc.write_text('accession\taccession.version\ttaxid\tgi\n'+''.join(f'r{i}\tr{i}\t{t}\t0\n' for i,t in enumerate(taxa)))
            bam=d/'input.bam';coord=d/'coord.bam';xr=d/'xr.bam'
            run('samtools','view','-b','-o',bam,sam)
            run('samtools','sort','-o',coord,bam)
            run('samtools','sort','-t','XR','-o',xr,bam)
            expected={}
            for tax in [2,3]:
                depths=[];total_length=0
                for ref, groups in enumerate(intervals):
                    if taxa[ref]!=tax: continue
                    total_length+=lengths[ref]
                    depth=[0]*max(e for s,e in groups)
                    for s,e in groups:
                        for pos in range(s,e): depth[pos]+=1
                    depths.extend(x for x in depth if x)
                mean=sum(depths)/len(depths)
                expected[str(tax)]=(len(depths),sum(depths)/total_length,mean,math.sqrt(sum((x-mean)**2 for x in depths)/len(depths)))
            for source in [bam,coord,xr]:
                for threads in [1,4]:
                    result=run(UNICORN,'taxstats','-b',source,'--names',names,'--nodes',nodes,'--acc2tax',acc,'--rank','genus','--threads',threads)
                    lines=result.stdout.splitlines();cols=[s.split(':',1)[1] for s in lines[0].split('\t')]
                    rows={v[0]:dict(zip(cols,v)) for v in (line.split('\t') for line in lines[1:])}
                    self.assertEqual(set(rows),set(expected))
                    for tax,(covered,mean,oncov,sd) in expected.items():
                        row=rows[tax]
                        self.assertEqual(int(row['ts_num_covbases']),covered)
                        self.assertAlmostEqual(float(row['ts_mean_cov']),mean,places=5)
                        self.assertAlmostEqual(float(row['ts_mean_covcovered']),oncov,places=5)
                        self.assertAlmostEqual(float(row['ts_stdev_covcovered']),sd,places=5)
