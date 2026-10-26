"""Independent ECDF oracle, order/spill invariance and circular rotations."""
import math,random
import test_outputs as base
from test_outputs import run,UNICORN

def parse(text):
 lines=text.strip().splitlines();names=[x.split(':',1)[1] for x in lines[0].split('\t')]
 return {r.split('\t')[0]:dict(zip(names,r.split('\t'))) for r in lines[1:]}
def oracle(xs):
 xs=sorted(xs);n=len(xs)
 if not n:return float('nan'),float('nan')
 dp=max((i+1)/n-x for i,x in enumerate(xs));dm=max(x-i/n for i,x in enumerate(xs))
 return max(dp,dm),dp+dm
class Spatial(base.Outputs):
 # Reuse the existing fixture without re-running its inherited tests.
 def test_spatial_oracle_orders_taxonomy_and_spill(self):
  rng=random.Random(18);L=1000
  pairs=[(rng.randrange(0,900),rng.randrange(20,100)) for _ in range(100)]+[(0,30),(970,30)]
  sam=self.path/'spatial.sam'
  with sam.open('w') as f:
   f.write('@HD\tVN:1.6\tSO:queryname\n@SQ\tSN:refA\tLN:1000\n')
   for i,(start,span) in enumerate(pairs):
    f.write(f'q{i:04}\t0\trefA\t{start+1}\t60\t{span}M\t*\t0\t0\t'+('ACGT'*25)[:span]+'\t'+('I'*span)+'\tNM:i:0\tAS:i:0\tXR:i:2\n')
  bam=self.path/'spatial.bam';coord=self.path/'spatial.coord.bam';xr=self.path/'spatial.xr.bam'
  run('samtools','view','-b','-o',bam,sam);run('samtools','sort','-o',coord,bam);run('samtools','sort','-t','XR','-o',xr,bam)
  ks,ku=oracle([a/(L-b) for a,b in pairs]);_,circ=oracle([a/L for a,b in pairs]);expected={'ks_linear':ks,'kuiper_linear':ku,'kuiper_circular':circ}
  for source in [bam,coord,xr]:
   for mem in [[],['--memory-limit','1M']]:
    r=parse(run(UNICORN,'refstats','-b',source,*mem).stdout)['refA']
    self.assertEqual(int(r['rs_spatial_nalns']),len(pairs));self.assertEqual(int(r['rs_spatial_invalid']),0)
    for key,value in expected.items():self.assertAlmostEqual(float(r['rs_'+key]),value,places=8)
    t=parse(run(UNICORN,'taxstats','-b',source,'--names',self.names,'--nodes',self.nodes,'--acc2tax',self.acc2tax,*mem).stdout)['2']
    for key,value in expected.items():
     for summary in ['mean','median','q25','q75']:self.assertAlmostEqual(float(t['ts_ref_'+summary+'_'+key]),value,places=8)
 def test_spatial_full_span_and_one_alignment(self):
  sam=self.path/'edge.sam';sam.write_text('@HD\tVN:1.6\tSO:coordinate\n@SQ\tSN:refA\tLN:32\n@SQ\tSN:refB\tLN:2000\n'+f'a\t0\trefA\t1\t60\t32M\t*\t0\t0\t'+('ACGT'*8)+'\t'+('I'*32)+'\tNM:i:0\tAS:i:0\n'+f'b\t0\trefB\t985\t60\t32M\t*\t0\t0\t'+('ACGT'*8)+'\t'+('I'*32)+'\tNM:i:0\tAS:i:0\n')
  bam=self.path/'edge.bam';run('samtools','view','-b','-o',bam,sam)
  rs=parse(run(UNICORN,'refstats','-b',bam).stdout)
  self.assertTrue(math.isnan(float(rs['refA']['rs_ks_linear'])))
  self.assertEqual(rs['refA']['rs_spatial_invalid'],'1')
  self.assertEqual(float(rs['refB']['rs_ks_linear']),.5)
  self.assertEqual(float(rs['refB']['rs_kuiper_linear']),1)
 def test_cigar_spans_and_undefined_tax_summary(self):
  sam=self.path/'cigar.sam'
  sam.write_text('@HD\tVN:1.6\tSO:unsorted\n@SQ\tSN:refA\tLN:1000\n@SQ\tSN:refB\tLN:32\n'+
   'a\t0\trefA\t101\t60\t2S5M3D5M2N5M1I\t*\t0\t0\t'+('ACGT'*5)[:18]+'\t'+('I'*18)+'\tNM:i:4\tAS:i:0\n'+
   'b\t0\trefB\t1\t60\t32M\t*\t0\t0\t'+('ACGT'*8)+'\t'+('I'*32)+'\tNM:i:0\tAS:i:0\n')
  bam=self.path/'cigar.bam';run('samtools','view','-b','-o',bam,sam)
  rs=parse(run(UNICORN,'refstats','-b',bam).stdout)
  self.assertAlmostEqual(float(rs['refA']['rs_ks_linear']),1-100/980,places=8)
  t=parse(run(UNICORN,'taxstats','-b',bam,'--names',self.names,'--nodes',self.nodes,'--acc2tax',self.acc2tax).stdout)['2']
  self.assertEqual(t['ts_ref_spatial_nvalid'],'1');self.assertEqual(t['ts_spatial_nalns'],'1');self.assertEqual(t['ts_spatial_invalid'],'1')
  self.assertAlmostEqual(float(t['ts_ref_mean_ks_linear']),1-100/980,places=8)
 def test_circular_rotation_invariance(self):
  vals=[]
  for shift in [0,100,375,800]:
   starts=[(x+shift)%1000 for x in [0,30,80,250,600,700]]
   sam=self.path/'rotation.sam'
   sam.write_text('@HD\tVN:1.6\tSO:unsorted\n@SQ\tSN:refA\tLN:1000\n'+''.join(f'q{i}\t0\trefA\t{x+1}\t60\t1M\t*\t0\t0\tA\tI\tNM:i:0\tAS:i:0\n' for i,x in enumerate(starts)))
   bam=self.path/'rotation.bam';run('samtools','view','-b','-o',bam,sam)
   vals.append(float(parse(run(UNICORN,'refstats','-b',bam).stdout)['refA']['rs_kuiper_circular']))
  self.assertLess(max(vals)-min(vals),1e-9)
# unittest discovers inherited tests too; leave them to the original class.
for name in list(base.Outputs.__dict__):
 if name.startswith('test_'):setattr(Spatial,name,None)
