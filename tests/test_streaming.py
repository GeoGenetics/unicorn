"""Scientific equivalence, bounded-buffer and failure-path regressions."""
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from test_outputs import run, UNICORN, ROOT


class Streaming(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='unicorn-stream-tests-')
        cls.path = Path(cls.temp.name)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def bam(self, name, records):
        sam = self.path / (name + '.sam')
        bam = self.path / (name + '.bam')
        with sam.open('w') as f:
            f.write('@HD\tVN:1.6\tSO:queryname\n@SQ\tSN:A\tLN:1000000\n@SQ\tSN:B\tLN:1000000\n')
            for q, ref, pos, length, nm in records:
                seq = ('ACGT' * ((length+3)//4))[:length]
                f.write(f'{q}\t0\t{ref}\t{pos}\t60\t{length}M\t*\t0\t0\t{seq}\t'+('I'*length)+f'\tNM:i:{nm}\tAS:i:0\n')
        run('samtools', 'view', '-b', '-o', bam, sam)
        return bam

    def filter(self, source, name, *args):
        out = self.path / (name + '.filtered.bam')
        run(UNICORN, 'alnfilt', '-b', source, '-o', out, '--minani', '93', '--maxani', '100', *args)
        run('samtools', 'quickcheck', out)
        return run('samtools', 'view', out).stdout.splitlines()

    def test_filter_modes_and_strict_bounds(self):
        records = [('a','A',1,100,3),('a','B',1,100,3),('a','B',101,100,5),
                   ('b','A',1,100,2),('b','B',1,100,10),('c','A',1,100,8)]
        bam = self.bam('modes', records)
        for threads in [1, 4]:
            for mode, expected in [('ALLTOP',3),('ALL',4),('PCTTOP',4),('RNDTOP',2)]:
                with self.subTest(threads=threads, mode=mode):
                    args = ['--mode',mode,'--threads',str(threads),'--seed','17']
                    rows = self.filter(bam, f'modes-{mode}-{threads}', *args)
                    self.assertEqual(len(rows), expected)
                    if mode == 'ALLTOP': self.assertEqual([r.split('\t')[0] for r in rows], ['a','a','b'])
            strict = self.filter(bam, f'strict-{threads}', '--strictbounds', '--mode','ALLTOP','--threads',str(threads))
            self.assertEqual([r.split('\t')[0] for r in strict], ['a','a'])

    def test_oversized_query_spill_matches_memory_and_threads(self):
        # A single highly multimapping query exceeds the forced 1 MiB budget.
        bam = self.bam('large-query', (('a','A',i+1,100,3) for i in range(12000)))
        for mode in ['ALLTOP','PCTTOP','ALL','RNDTOP']:
            a = self.filter(bam, mode+'-memory','--mode',mode,'--seed','9','--group-memory','64','--threads','1')
            b = self.filter(bam, mode+'-spill','--mode',mode,'--seed','9','--group-memory','1','--threads','4')
            self.assertEqual(a,b)

    def test_empty_and_all_rejected_inputs_are_valid_bams(self):
        self.assertEqual(self.filter(self.bam('empty',[]),'empty'), [])
        self.assertEqual(self.filter(self.bam('rejected',[('q','A',1,100,10)]),'rejected'), [])

    def test_wide_nm_does_not_wrap(self):
        bam=self.bam('wide-nm',[('q','A',1,1000,256)])
        self.assertEqual(self.filter(bam,'wide-nm'), [])

    def test_bam_stdin_and_stdout(self):
        bam=self.bam('pipes',[('q','A',1,100,3)])
        with bam.open('rb') as source:
            p=subprocess.run([UNICORN,'alnfilt','-b','-','--minani','93','--maxani','100','--compression','0'],
                             stdin=source,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True)
        out=self.path/'pipe.bam';out.write_bytes(p.stdout)
        run('samtools','quickcheck',out)
        self.assertEqual(int(run('samtools','view','-c',out).stdout),1)

    def test_bad_options_and_truncated_bam_fail(self):
        bam=self.bam('bad',[('q','A',1,100,3)])
        for options in [['--threads','0'],['--compression','10'],['--group-memory','0'],['--minani','99','--maxani','93']]:
            with self.subTest(options=options):
                p=subprocess.run([UNICORN,'alnfilt','-b',str(bam),'-o',str(self.path/'bad-output.bam'),*options],capture_output=True,timeout=15)
                self.assertNotEqual(p.returncode,0)
        truncated=self.path/'truncated.bam';data=bam.read_bytes();truncated.write_bytes(data[:len(data)//2])
        p=subprocess.run([UNICORN,'alnfilt','-b',str(truncated),'-o',str(self.path/'truncated-output.bam')],capture_output=True,timeout=15)
        self.assertNotEqual(p.returncode,0)
        target = '/dev/full' if Path('/dev/full').exists() else str(self.path)
        p=subprocess.run([UNICORN,'alnfilt','-b',str(bam),'-o',target],capture_output=True,timeout=15)
        self.assertNotEqual(p.returncode,0)

    def test_tmpdir_and_truncated_body(self):
        bam=self.bam('body',((f'q{i:08d}','A',i+1,100,3) for i in range(5000)))
        target=self.path/'body-truncated.bam';data=bam.read_bytes();target.write_bytes(data[:len(data)//2])
        p=subprocess.run([UNICORN,'alnfilt','-b',str(target),'-o',str(self.path/'body-out.bam')],capture_output=True,timeout=15)
        self.assertNotEqual(p.returncode,0)
        p=subprocess.run([UNICORN,'alnfilt','-b',str(bam),'--tmpdir',str(self.path/'missing-directory')],capture_output=True,timeout=15)
        self.assertNotEqual(p.returncode,0)
        self.filter(bam,'explicit-scratch','--tmpdir',str(self.path))

    def test_reference_disk_spill_matches_memory(self):
        bam=self.bam('ref-spill', ((f'q{i:08d}','A',i+1,30+i%25,i%2) for i in range(70000)))
        coord=self.path/'ref-spill.coord.bam';run('samtools','sort','-o',coord,bam)
        reference=None
        for memory,threads in [('64','1'),('1','1'),('1','4')]:
            out=self.path/f'ref-spill-{memory}-{threads}.bam'
            result=run(UNICORN,'refstats','-b',coord,'--group-memory',memory,'--threads',threads,'-o',out,'--qsize','7')
            run('samtools','quickcheck',out)
            self.assertEqual(int(run('samtools','view','-c',out).stdout),70000)
            if reference is None: reference=result.stdout
            else: self.assertEqual(result.stdout,reference)

    def test_short_reads_and_dust_filter_output(self):
        bam=self.bam('short-reads',[(f'q{i}','A',1+i*5,i,0) for i in [1,2,3]])
        result=run(UNICORN,'refstats','-b',bam)
        self.assertEqual(len(result.stdout.splitlines()),2)
        source=self.path/'dust.sam';bam=self.path/'dust.bam'
        rng=random.Random(42);high=''.join(rng.choices('ACGT',k=100))
        with source.open('w') as f:
            f.write('@HD\tVN:1.6\tSO:queryname\n@SQ\tSN:A\tLN:1000\n')
            for i,seq in enumerate([high,'A'*100,high,'C'*100]):
                f.write(f'q{i}\t0\tA\t{i+1}\t60\t100M\t*\t0\t0\t{seq}\t'+('I'*100)+'\tNM:i:0\tAS:i:0\n')
        run('samtools','view','-b','-o',bam,source)
        out=self.path/'dust.filtered.bam'
        run(UNICORN,'refstats','-b',bam,'--maxdust','5','-o',out)
        self.assertEqual(int(run('samtools','view','-c',out).stdout),2)

    def test_unmapped_tail_and_empty_reference_dictionary(self):
        source=self.path/'unmapped-tail.sam';bam=self.path/'unmapped-tail.bam'
        source.write_text('@HD\tVN:1.6\tSO:coordinate\n@SQ\tSN:A\tLN:1000\n'
                          'a\t0\tA\t10\t60\t4M\t*\t0\t0\tACGT\tIIII\tNM:i:0\tAS:i:0\n'
                          'b\t4\t*\t0\t0\t*\t*\t0\t0\tACGT\tIIII\n')
        run('samtools','view','-b','-o',bam,source)
        out=self.path/'unmapped-tail.filtered.bam'
        result=run(UNICORN,'refstats','-b',bam,'-o',out,'--threads','4')
        self.assertEqual(len(result.stdout.splitlines()),2)
        self.assertEqual(int(run('samtools','view','-c',out).stdout),1)
        source=self.path/'bare-empty.sam';source.write_text('@HD\tVN:1.6\tSO:queryname\n')
        bam=self.path/'bare-empty.bam';run('samtools','view','-b','-o',bam,source)
        out=self.path/'bare-empty.filtered.bam';run(UNICORN,'alnfilt','-b',bam,'-o',out)
        run('samtools','quickcheck','-u',out)
        self.assertEqual(int(run('samtools','view','-c',out).stdout),0)

    def test_mislabeled_coordinate_input_fails(self):
        source=self.path/'misordered.sam'
        source.write_text('@HD\tVN:1.6\tSO:coordinate\n@SQ\tSN:A\tLN:1000\n'
                          'a\t0\tA\t100\t60\t4M\t*\t0\t0\tACGT\tIIII\tNM:i:0\tAS:i:0\n'
                          'b\t0\tA\t10\t60\t4M\t*\t0\t0\tACGT\tIIII\tNM:i:0\tAS:i:0\n')
        p=subprocess.run([UNICORN,'refstats','-b',str(source)],capture_output=True,timeout=15)
        self.assertNotEqual(p.returncode,0)
        self.assertIn(b'not coordinate sorted',p.stderr)

    def test_kmer_map_spill_with_diverse_sequences(self):
        sam=self.path/'diverse.sam';bam=self.path/'diverse.bam'
        rng=random.Random(19)
        with sam.open('w') as f:
            f.write('@HD\tVN:1.6\tSO:coordinate\n@SQ\tSN:A\tLN:1000000\n')
            for i in range(5000):
                seq=''.join(rng.choices('ACGT',k=100))
                f.write(f'q{i:06d}\t0\tA\t{i+1}\t60\t100M\t*\t0\t0\t{seq}\t'+('I'*100)+'\tNM:i:0\tAS:i:0\n')
        run('samtools','view','-b','-o',bam,sam)
        a=run(UNICORN,'refstats','-b',bam,'--group-memory','64','--threads','1').stdout
        b=run(UNICORN,'refstats','-b',bam,'--group-memory','1','--threads','4','--tmpdir',self.path).stdout
        self.assertEqual(a,b)

    def test_randomized_filter_oracle(self):
        import struct
        f32=lambda x: struct.unpack('f',struct.pack('f',x))[0]
        rng=random.Random(23); records=[]; groups=[]
        for i in range(80):
            group=[]
            for j in range(rng.randint(1,12)):
                record=(f'q{i:06d}',rng.choice(['A','B']),1+j*101,100,rng.randint(0,14))
                records.append(record); group.append(record)
            groups.append(group)
        bam=self.bam('randomized',records)
        for mode in ['ALLTOP','ALL','PCTTOP']:
            for strict in [False,True]:
                expected=[]
                for group in groups:
                    scores=[f32((1.0-f32(r[4]/r[3]))*100.0) for r in group]
                    if strict and any(x<93 or x>100 for x in scores): continue
                    valid=[(r,x) for r,x in zip(group,scores) if 93<=x<=100]
                    if not valid: continue
                    best=max(x for _,x in valid)
                    for r,x in valid:
                        if mode=='ALL' or (mode=='ALLTOP' and x==best) or (mode=='PCTTOP' and x>=f32(best*f32(0.99))):
                            expected.append((r[0],r[1],r[2],r[4]))
                args=['--mode',mode,'--pct','0.99','--threads','4']
                if strict: args+=['--strictbounds']
                rows=self.filter(bam,f'oracle-{mode}-{strict}',*args)
                actual=[]
                for row in rows:
                    fields=row.split('\t'); nm=int(next(x[5:] for x in fields[11:] if x.startswith('NM:i:')))
                    actual.append((fields[0],fields[2],int(fields[3]),nm))
                self.assertEqual(actual,expected)

    def test_64bit_counters_and_spill_allocator(self):
        source=self.path/'large-counters.c'
        source.write_text(r'''#include <stdint.h>
#include <string.h>
#include "unicorn_internal.h"
extern int64_t unicorn_getnqueries(unicorn_t *u);
extern int64_t unicorn_getnfqueries(unicorn_t *u);
int main(void) {
 unicorn_t u={0}; u.values.nread=(1ULL<<33)+7; u.values.nfread=(1ULL<<32)+9;
 if(unicorn_getnqueries(&u)!=8589934599LL || unicorn_getnfqueries(&u)!=4294967305LL) return 1;
 readhist_t hist={0}; hist.n=1;hist.lengths[0]=100;hist.counts[0]=1ULL<<33;
 if(readhist_median(&hist,1ULL<<33)!=100 || readhist_mode(&hist)!=100) return 2;
 unicorn_spill_threshold=128;
 for(int round=0;round<10;round++) {
  unsigned char *p=unicorn_mem_alloc(&unicorn_spill_threshold,64,1);
  if(!p) return 3;
  for(int i=0;i<64;i++) {if(p[i])return 4;p[i]=i;}
  p=unicorn_mem_resize(&unicorn_spill_threshold,p,4096);
  if(!p) return 5;
  for(int i=0;i<64;i++) if(p[i]!=i)return 6;
  p=unicorn_mem_resize(&unicorn_spill_threshold,p,8192);
  if(!p) return 7;
  for(int i=0;i<64;i++) if(p[i]!=i)return 8;
  p=unicorn_mem_resize(&unicorn_spill_threshold,p,16);
  if(!p)return 9;
  for(int i=0;i<16;i++)if(p[i]!=i)return 10;
  unicorn_mem_free(&unicorn_spill_threshold,p);
 }
 lint2int_t *map=lint2int_init2(&unicorn_spill_threshold);int absent;
 khint_t k=lint2int_put(map,17,&absent);kh_val(map,k)=(1ULL<<33)+1;
 if(kh_val(map,lint2int_get(map,17))!=8589934593ULL)return 11;
 lint2int_destroy(map);
 unicorn_readset_t *names=unicorn_readset_new(0);
 if(!names)return 12;
 if(unicorn_readset_add(names,"reade1b024b95172fb60",0)!=1)return 13;
 if(unicorn_readset_add(names,"read79b3d9f3f6a4854a",0)!=1)return 14;
 if(unicorn_readset_add(names,"reade1b024b95172fb60",0)!=0)return 15;
 if(unicorn_readset_size(names)!=2)return 16;
 unicorn_readset_free(names);
 names=unicorn_readset_new(1);
 if(!names)return 17;
 if(unicorn_readset_add(names,"a",1ULL<<33)!=1)return 18;
 if(unicorn_readset_add(names,"b",(1ULL<<33)+1)!=1)return 19;
 if(unicorn_readset_add(names,"a",1ULL<<33)!=0)return 20;
 if(unicorn_readset_size(names)!=2)return 21;
 unicorn_readset_free(names);return 0;
}''')
        executable=self.path/'large-counters'
        prefix=Path(os.environ.get('CONDA_PREFIX','/usr/local'))
        flags=[]
        if os.environ.get('UNICORN_SANITIZE_TESTS'):
            flags=['-arch','x86_64','-fsanitize=address,undefined'] if sys.platform=='darwin' else ['-fsanitize=address,undefined']
        run((os.environ.get('CC') or shutil.which('clang') or shutil.which('cc') or 'cc'),*flags,'-I'+str(ROOT/'src'),'-I'+str(ROOT/'src/libunicorn'),
            '-I'+str(prefix/'include'),source,ROOT/'libunicorn.a',ROOT/'src/genesisC/genesisC.o',
            '-L'+str(prefix/'lib'),'-Wl,-rpath,'+str(prefix/'lib'),'-lhts','-lz','-lm','-lpthread',
            ('-lc++' if sys.platform=='darwin' else '-lstdc++'),'-o',executable)
        run(executable)

    def test_kmer_rolling_matches_original_encoder(self):
        # Directly test every k, including invalid windows and reverse complements.
        source=self.path/'kmers.c'
        source.write_text('''#include <stdint.h>
#include <stdio.h>
#include "genesisC.h"
int main(void) {
 const char seq[]="ACGTACGTNACGTACGTACGTACGTACGTACGTACGTTTTTACCCCCGGGGAAT";
 for(int k=1;k<=32;k++) {
  genesis_encoder_t e=genesis_encoderinit(k); uint64_t values[128];
  genesis_getcamexidx_bulk(e,seq,sizeof(seq)-1,k,values);
  for(int i=0;i<=((int)sizeof(seq)-1)-k;i++) {uint8_t ret;
   uint64_t old=genesis_getcamexidx(e,seq+i,k,&ret);
   if(old!=values[i]) return 1;
  } genesis_encoderfree(e);
 } return 0;
}''')
        executable=self.path/'kmers'
        arch = ['-arch','x86_64'] if os.environ.get('UNICORN_SANITIZE_TESTS') and sys.platform=='darwin' else []
        run((os.environ.get('CC') or shutil.which('clang') or shutil.which('cc') or 'cc'),*arch,'-I'+str(ROOT/'src/genesisC'),source,
            ROOT/'src/genesisC/genesisC.o',('-lc++' if sys.platform == 'darwin' else '-lstdc++'),'-o',executable)
        run(executable)


if __name__=='__main__': unittest.main()
