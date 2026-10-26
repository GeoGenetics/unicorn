from pathlib import Path
import urllib.request,time,json,re,os
p=Path(os.environ.get('UNICORN_BENCHMARK_DIR',Path.cwd()/'filter_benchmark_results'));p.mkdir(parents=True,exist_ok=True)
ids=['OZ037764.1','MZ365446.1','MW435444.1','MW435428.1','NC_044419.1','NC_068759.1','MT872525.1','MW537664.1']
meta=[]
for acc in ids:
 f=p/(acc+'.gb')
 if not f.exists():
  url=f'https://www.ncbi.nlm.nih.gov/sviewer/viewer.fcgi?id={acc}&db=nuccore&report=genbank&retmode=text'
  for attempt in range(3):
   try:f.write_bytes(urllib.request.urlopen(url,timeout=60).read());break
   except Exception:
    if attempt==2:raise
    time.sleep(2)
  time.sleep(.5)
 t=f.read_text();assert 'ORIGIN' in t and ('chloroplast' in t.lower() or 'plastid' in t.lower()),acc
 seq=''.join(re.findall('[acgtACGTnN]+',t.split('ORIGIN',1)[1].split('//')[0])).upper()
 org=re.search(r'ORGANISM\s+([^\n]+)',t).group(1).strip()
 (p/(acc+'.fa')).write_text('>'+acc+'\n'+seq+'\n')
 meta.append(dict(accession=acc,organism=org,length=len(seq)))
 print(meta[-1],flush=True)
(p/'reference_metadata.json').write_text(json.dumps(meta[:6],indent=2))
