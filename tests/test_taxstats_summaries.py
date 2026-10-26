import csv,gzip,math,random,statistics,subprocess,tempfile,unittest
from pathlib import Path
from test_outputs import UNICORN,run

def table(text):
 lines=text.splitlines();cols=[s.split(':',1)[1] for s in lines[0].split('\t')]
 return [dict(zip(cols,line.split('\t'))) for line in lines[1:]]
def quantile(x,q):
 x=sorted(x);p=(len(x)-1)*q;i=int(p)
 return x[i]+(p-i)*(x[min(i+1,len(x)-1)]-x[i])

class ReferenceSummaries(unittest.TestCase):
 def test_reference_counterparts_medians_lengths_and_routes(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);lengths={'cp':200,'nucA':5000,'nucB':10000,'absent':7000,'singleA':4000,'singleB':4000}
   taxa={x:2 if x in ['cp','nucA','nucB','absent'] else 3 for x in lengths}
   records=[]
   for i in range(500):records.append((f'cp{i}','cp',1+i%160,32,i%2))
   records.extend([('long201','nucA',1000,201,2),('long255','nucA',2000,255,3),('long300','nucA',3000,300,4),('long400','nucB',10,400,5),('oddA','singleA',1,101,0),('oddB','singleB',1,102,3)])
   sam=p/'a.sam'
   with sam.open('w') as f:
    f.write('@HD\tVN:1.6\tSO:unsorted\n'+''.join(f'@SQ\tSN:{ref}\tLN:{length}\n' for ref,length in lengths.items()))
    for name,ref,pos,n,nm in records:
     seq=('ACGT'*((n+3)//4))[:n]
     f.write(f'{name}\t0\t{ref}\t{pos}\t60\t{n}M\t*\t0\t0\t{seq}\t'+('I'*n)+f'\tNM:i:{nm}\tXR:i:{taxa[ref]}\n')
   bam=p/'a.bam';coord=p/'coord.bam';xr=p/'xr.bam'
   run('samtools','view','-b','-o',bam,sam);run('samtools','sort','-o',coord,bam);run('samtools','sort','-t','XR','-o',xr,bam)
   names=p/'names';nodes=p/'nodes';acc=p/'acc'
   names.write_text(''.join(f'{i}\t|\tname{i}\t|\t\t|\tscientific name\t|\n' for i in [1,2,3]));nodes.write_text('1\t|\t1\t|\tno rank\t|\n2\t|\t1\t|\tgenus\t|\n3\t|\t1\t|\tgenus\t|\n');acc.write_text('accession\taccession.version\ttaxid\tgi\n'+''.join(f'{r}\t{r}\t{t}\t0\n' for r,t in taxa.items()))
   refs=table(run(UNICORN,'refstats','-b',coord).stdout)
   mapping={'breadth':'rs_breath_cov','mean_cov':'rs_mean_cov','mean_covcovered':'rs_mean_covcovered','evenness_cov':'rs_evenness_cov','breadth_ratio':'rs_breath_ratio','entropy':'rs_entropy','gini':'rs_gini','norm_entropy':'rs_norm_entropy','norm_gini':'rs_norm_gini','ks_linear':'rs_ks_linear','kuiper_linear':'rs_kuiper_linear','kuiper_circular':'rs_kuiper_circular'}
   expected_lengths={t:{} for t in [2,3]}
   for name,ref,pos,n,nm in records:expected_lengths[taxa[ref]][n]=expected_lengths[taxa[ref]].get(n,0)+1
   for source in [bam,coord,xr]:
    for threads in [1,4]:
     for spill in [[],['--memory-limit','1M']]:
      out=p/'lengths.tsv.gz'
      rows=table(run(UNICORN,'taxstats','-b',source,'--names',names,'--nodes',nodes,'--acc2tax',acc,'--threads',threads,'--read-lengths',out,*spill).stdout)
      observed={}
      for row in csv.DictReader(gzip.open(out,'rt'),delimiter='\t'):observed.setdefault(int(row['taxid']),{})[int(row['length'])]=int(row['count'])
      self.assertEqual(observed,expected_lengths)
      for row in rows:
       t=int(row['taxid']);group=[r for r in refs if taxa[r['Id']]==t]
       self.assertEqual(int(row['ts_num_accessions']),len(group))
       for metric,column in mapping.items():
        values=[float(r[column]) for r in group]
        for name,want in [('mean',statistics.mean(values)),('median',statistics.median(values)),('q25',quantile(values,.25)),('q75',quantile(values,.75))]:
         got=float(row[f'ts_ref_{name}_{metric}']);self.assertTrue(math.isclose(got,want,abs_tol=2e-6,rel_tol=2e-6),(metric,name,got,want))
       readlens=[n for name,ref,pos,n,nm in records if taxa[ref]==t]
       nms=[nm for name,ref,pos,n,nm in records if taxa[ref]==t]
       self.assertEqual(float(row['ts_median_readl']),statistics.median(readlens))
       self.assertEqual(float(row['ts_median_alnnm']),statistics.median(nms))
       self.assertEqual(int(row['ts_nreads']),sum(observed[t].values()))
       covered=sum(int(r['rs_num_covbases']) for r in group);denom=sum(lengths[r['Id']]*(1-math.exp(-float(r['rs_mean_cov']))) for r in group)
       self.assertAlmostEqual(float(row['ts_breath_ratio']),covered/denom,places=5)
       dust = float(next(r for r in group if r['Id']=='cp')['rs_mdust']) if t==2 else statistics.median(float(r['rs_mdust']) for r in group)
       self.assertAlmostEqual(float(row['ts_median_dust']),dust,places=6)
       if t==2:self.assertLess(float(row['ts_ref_median_breadth']),.2)
   # Automatic companion with a named, BGZF main table.
   run(UNICORN,'taxstats','-b',coord,'--names',names,'--nodes',nodes,'--acc2tax',acc,'--outstat',p/'summary.tsv')
   self.assertTrue((p/'summary.readlengths.tsv.gz').exists())
