import gzip,subprocess,tempfile,unittest,os
from pathlib import Path
from test_outputs import UNICORN,run

class Storage(unittest.TestCase):
 def test_compression_and_rss_spill_preserve_results(self):
  with tempfile.TemporaryDirectory() as td:
   p=Path(td);sam=p/'in.sam';bam=p/'in.bam';xr=p/'xr.bam';names=p/'names';nodes=p/'nodes';acc=p/'acc'
   with sam.open('w') as f:
    f.write('@HD\tVN:1.6\tSO:coordinate\n@SQ\tSN:A\tLN:1000000\n')
    for i in range(20000):f.write(f'q{i%400}\t0\tA\t{i+1}\t60\t32M\t*\t0\t0\t'+('ACGT'*8)+'\t'+('I'*32)+'\tNM:i:0\tXR:i:2\n')
   run('samtools','view','-b','-o',bam,sam);run('samtools','sort','-t','XR','-o',xr,bam)
   names.write_text('1\t|\troot\t|\t\t|\tscientific name\t|\n2\t|\tTest\t|\t\t|\tscientific name\t|\n');nodes.write_text('1\t|\t1\t|\tno rank\t|\n2\t|\t1\t|\tgenus\t|\n');acc.write_text('accession\taccession.version\ttaxid\tgi\nA\tA\t2\t0\n')
   def cli(*args):
    r=subprocess.run([str(UNICORN),*map(str,args)],capture_output=True,timeout=120)
    self.assertEqual(r.returncode,0,r.stderr.decode());return r
   for step,source in [('refstats',bam),('taxstats',bam),('taxstats',xr)]:
    opts=[step,'-b',source,'--threads','2']
    if step=='taxstats':opts+=['--names',names,'--nodes',nodes,'--acc2tax',acc]
    expected=cli(*opts).stdout
    base=p/f'{step}-{source.stem}.tsv'
    cli(*opts,'--outstat',base)
    zipped=Path(str(base)+'.gz');self.assertFalse(base.exists());self.assertEqual(gzip.decompress(zipped.read_bytes()),expected)
    self.assertEqual(zipped.read_bytes()[12:14],b'BC')
    cli(*opts,'--outstat',zipped)
    self.assertFalse(Path(str(zipped)+'.gz').exists())
    self.assertEqual(gzip.decompress(zipped.read_bytes()),expected)
    low=cli(*opts,'--memory-limit','1M','--tmpdir',p)
    self.assertIn(b'RSS spill threshold reached',low.stderr);self.assertEqual(low.stdout,expected)
    cli(*opts,'--plain-text','--outstat',base);self.assertEqual(base.read_bytes(),expected)
   files=p/'list.txt';files.write_text(str(bam)+'\n'+str(xr)+'\n')
   combined=p/'combined.tsv'
   cli('taxstats','--filelist',files,*opts[3:],'--outstat',combined)
   self.assertEqual(gzip.decompress(Path(str(combined)+'.gz').read_bytes()),expected+expected)
   a=p/'a.bam';b=p/'b.bam'
   # Query-group input for filtering.
   query=p/'query.bam';run('samtools','sort','-n','-o',query,bam)
   cli('alnfilt','-b',query,'-o',a,'--mode','ALLTOP')
   forced=cli('alnfilt','-b',query,'-o',b,'--mode','ALLTOP','--memory-limit','1M','--tmpdir',p)
   self.assertIn(b'RSS spill threshold reached',forced.stderr)
   self.assertEqual(run('samtools','view',a).stdout,run('samtools','view',b).stdout)
   self.assertFalse(list(p.glob('unicorn-*')))
 def test_invalid_memory_limits(self):
  for value in ['0','-1','nan','inf','1Q','18446744073709551616']:
   p=subprocess.run([str(UNICORN),'refstats','--memory-limit',value],capture_output=True)
   self.assertNotEqual(p.returncode,0,value)
