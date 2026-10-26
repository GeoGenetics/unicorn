"""Reproducible known-origin pilot. Dependencies: bowtie2, samtools, Unicorn, Python3.
Override UNICORN/BOWTIE/SAMTOOLS to reuse with other builds. Seed and settings recorded.
"""
from pathlib import Path
import os,json,re,random,subprocess,csv,time,hashlib
P=Path(os.environ.get('UNICORN_BENCHMARK_DIR',Path.cwd()/'filter_benchmark_results'));U=os.environ.get('UNICORN',str(Path(__file__).resolve().parents[2]/'unicorn'));B=os.environ.get('BOWTIE','bowtie2');S=os.environ.get('SAMTOOLS','samtools')
meta=json.loads((P/'reference_metadata.json').read_text());salix=[x['accession'] for x in meta];seq={a:(P/(a+'.fa')).read_text().split('\n')[1] for a in salix}
for a in ['MT872525.1','MW537664.1']:
 f=P/(a+'.gb');t=f.read_text();seq[a]=''.join(re.findall('[acgtACGTnN]+',t.split('ORIGIN',1)[1].split('//')[0])).upper();(P/(a+'.fa')).write_text('>'+a+'\n'+seq[a]+'\n')
allrefs=list(seq);(P/'logs').mkdir(exist_ok=True);(P/'samples').mkdir(exist_ok=True)
def run(args,out=None,log='commands'):
 with (P/'logs'/f'{log}.log').open('a') as err:
  err.write('\nCOMMAND '+repr(list(map(str,args)))+'\n');err.flush()
  if out:
   with Path(out).open('w') as f:subprocess.run(list(map(str,args)),stdout=f,stderr=err,check=True)
  else:subprocess.run(list(map(str,args)),stdout=err,stderr=err,check=True)
for missing in [None]+salix:
 key=missing or 'all';fa=P/(key+'.database.fa');idx=P/(key+'.index')
 fa.write_text(''.join('>'+a+'\n'+seq[a]+'\n' for a in allrefs if a!=missing))
 if not Path(str(idx)+'.1.bt2').exists():run([B+'-build',fa,idx],log='index')
rows=[];truth=[];starttime=time.time()
for si,source in enumerate(salix):
 for rep in range(3):
  for scenario in ['single','rare_relative','source_missing','background_only']:
   sid=f's{si}_r{rep}_{scenario}';out=P/'samples'/sid;out.mkdir(exist_ok=True)
   source_counts={allrefs[-2]:100,allrefs[-1]:100}
   if scenario!='background_only':source_counts[source]=600
   if scenario=='rare_relative':
    # Pair within train or holdout partition to avoid source leakage.
    partner=salix[(si+1)%4] if si<4 else salix[9-si]
    source_counts[partner]=20
   truth.append(dict(sample=sid,source=source,split='train' if si<4 else 'test',scenario=scenario,replicate=rep,damage=[0,.05,.15][rep],present=list(source_counts),counts=source_counts))
   rng=random.Random(241024+100*si+10*rep+['single','rare_relative','source_missing','background_only'].index(scenario));fq=out/'reads.fq'
   if not fq.exists():
    with fq.open('w') as f:
     n=0
     for acc,count in source_counts.items():
      prior=[]
      for i in range(count):
       if prior and rng.random()<.10:read=rng.choice(prior)
       else:
        length=rng.randint(30,80);j=rng.randrange(len(seq[acc]));read=(seq[acc]+seq[acc][:80])[j:j+length]
        if rng.random()<.5:read=read.translate(str.maketrans('ACGT','TGCA'))[::-1]
        bases=list(read)
        for k,b in enumerate(bases):
         if rng.random()<.005:bases[k]=rng.choice([x for x in 'ACGT' if x!=b])
        damage=[0,.05,.15][rep]
        for k in range(min(5,len(bases))):
         if bases[k]=='C' and rng.random()<damage*(.65**k):bases[k]='T'
         if bases[-k-1]=='G' and rng.random()<damage*(.65**k):bases[-k-1]='A'
        read=''.join(bases);prior.append(read)
       f.write(f'@read{n:06}\n{read}\n+\n'+('I'*len(read))+'\n');n+=1
   raw=out/'raw.sam';idx=P/((source if scenario=='source_missing' else 'all')+'.index')
   if not raw.exists():run([B,'-x',idx,'-U',fq,'-S',raw,'-p','2','-k','1000','-D','15','-R','2','-N','1','-L','22','-i','S,1,1.15','--np','1','--mp','1,1','--rdg','0,1','--rfg','0,1','--score-min','L,0,-0.1','--seed','24'],log=sid)
   query=out/'query.bam'
   if not query.exists():run([S,'sort','-n','-o',query,raw],log=sid)
   for ani in [93,97,99,100]:
    filt=out/f'ani{ani}.bam';coord=out/f'ani{ani}.coord.bam'
    if not coord.exists():
     run([U,'alnfilt','-b',query,'-o',filt,'--mode','ALLTOP','--minani',ani,'--maxani','100'],log=sid)
     run([S,'sort','-o',coord,filt],log=sid)
    for dust in [20,100]:
     stats=out/f'ani{ani}.dust{dust}.tsv'
     if not stats.exists():run([U,'refstats','-b',coord,'--maxdust',dust],out=stats,log=sid)
     lines=stats.read_text().strip().splitlines();cols=[x.split(':',1)[1] for x in lines[0].split('\t')]
     for line in lines[1:]:
      r=dict(zip(cols,line.split('\t')));r.update(sample=sid,source=source,split='train' if si<4 else 'test',scenario=scenario,replicate=rep,ani_filter=ani,dust_filter=dust,true_present=int(r['Id'] in source_counts));rows.append(r)
   print(sid,'complete',round(time.time()-starttime,1),'s',flush=True)
(P/'truth.json').write_text(json.dumps(truth,indent=2))
with (P/'features.tsv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=list(rows[0]),delimiter='\t');w.writeheader();w.writerows(rows)
(P/'run_manifest.json').write_text(json.dumps(dict(seed=241024,mapper=B,unicorn=U,samtools=S,seconds=time.time()-starttime,samples=len(truth),rows=len(rows),sha256={str(f.name):hashlib.sha256(f.read_bytes()).hexdigest() for f in P.glob('*.fa')}),indent=2))
