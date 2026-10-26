"""Transparent conjunctive threshold search with source-species holdout.
No classifier, learned probability, or calibrated FDR claim is made.
"""
from pathlib import Path
import csv,json,itertools,math,os,gzip
import numpy as np
P=Path(os.environ.get('UNICORN_BENCHMARK_DIR',Path.cwd()/'filter_benchmark_results'))
truth=json.loads((P/'truth.json').read_text());features=(P/'features.tsv').open() if (P/'features.tsv').exists() else gzip.open(P/'features.tsv.gz','rt');rows=list(csv.DictReader(features,delimiter='\t'));salix=[x['accession'] for x in json.loads((P/'reference_metadata.json').read_text())]
samples=[r['sample'] for r in truth];keys=[(s,a) for s in samples for a in salix];index={k:i for i,k in enumerate(keys)}
y=np.array([a in t['present'] for t in truth for a in salix]);train=np.array([t['split']=='train' for t in truth for a in salix]);test=~train
configs={}
for ani,dust in itertools.product([93,97,99,100],[20,100]):
 X=np.full((len(keys),5),np.nan)
 for r in rows:
  if int(r['ani_filter'])!=ani or int(r['dust_filter'])!=dust or r['Id'] not in salix:continue
  X[index[(r['sample'],r['Id'])]]=[float(r[k]) for k in ['rs_nreads','rs_norm_entropy','rs_duplicity','rs_ks_linear','rs_spatial_nalns']]
 configs[ani,dust]=X

def metrics(pred,mask):
 tp=int(np.sum(pred&y&mask));fp=int(np.sum(pred&~y&mask));fn=int(np.sum(~pred&y&mask));n=int(np.sum(mask)//6)
 return dict(tp=tp,fp=fp,fn=fn,precision=tp/(tp+fp) if tp+fp else None,recall=tp/(tp+fn) if tp+fn else None,false_calls_per_sample=fp/n if n else None)
def predict(c):
 X=configs[c['ani'],c['dust']];n,e,d,ks,na=X.T
 ent=np.ones(len(n),dtype=bool) if c['entropy_rule']=='off' else (e>=c['entropy'] if c['entropy_rule']=='min' else e<=c['entropy'])
 spatial=ks if c['ks_mode']=='raw' else ks*np.sqrt(na)
 return (n>=c['nreads'])&ent&(d>=c['duplicity'])&(spatial<=c['ks'])
choices=[]
for ani,dust,n,ent,dup,ks in itertools.product([93,97,99,100],[20,100],[3,5,10,20,50,100,300,500,700],[('off',0),('min',.25),('min',.5),('min',.75),('max',.5),('max',.75)],[0,.1,.3,.5,.7],[('raw',1),('raw',.025),('raw',.05),('raw',.075),('raw',.1),('raw',.2),('scaled',1),('scaled',1.36),('scaled',2)]):
 choices.append(dict(ani=ani,dust=dust,nreads=n,entropy_rule=ent[0],entropy=ent[1],duplicity=dup,ks_mode=ks[0],ks=ks[1]))
# Refine around observed TRAINING values only; keep the test species untouched.
rng=np.random.default_rng(241024)
for _ in range(30000):
 ani=int(rng.choice([93,97,99,100]));dust=int(rng.choice([20,100]));x=configs[ani,dust][train];x=x[np.isfinite(x).all(axis=1)]
 n=int(np.quantile(x[:,0],rng.choice(np.linspace(0,1,21))))
 rule=str(rng.choice(['off','min','max']));ent=float(np.quantile(x[:,1],rng.choice(np.linspace(0,1,21))))
 dup=0 if rng.random()<.3 else float(np.quantile(x[:,2],rng.choice(np.linspace(0,1,21))))
 mode=str(rng.choice(['raw','scaled']));dist=x[:,3] if mode=='raw' else x[:,3]*np.sqrt(x[:,4])
 ks=1 if mode=='raw' and rng.random()<.3 else float(np.quantile(dist,rng.choice(np.linspace(0,1,21))))
 choices.append(dict(ani=ani,dust=dust,nreads=max(3,n),entropy_rule=rule,entropy=ent,duplicity=dup,ks_mode=mode,ks=ks))
base=dict(ani=93,dust=100,nreads=3,entropy_rule='off',entropy=0,duplicity=0,ks_mode='raw',ks=1)
results=[];frontier=[]
for variant in ['all','without_ks','without_entropy','without_duplicity','without_dust']:
 best=None;bestkey=None;explore=None;explorekey=None
 for original in choices:
  c=dict(original)
  if variant=='without_ks':c.update(ks_mode='raw',ks=1)
  if variant=='without_entropy':c.update(entropy_rule='off',entropy=0)
  if variant=='without_duplicity':c['duplicity']=0
  if variant=='without_dust':c['dust']=100
  pred=predict(c);m=metrics(pred,train)
  if variant=='all':frontier.append({**c,**m})
  if m['precision'] is not None:
   ek=(m['precision'],m['recall'])
   if explorekey is None or ek>explorekey:explore,explorekey=c,ek
  if m['precision'] is None or m['precision']<.95:continue
  # Prefer sensitivity, then precision, then simpler permissive filters.
  key=(m['recall'],m['precision'],int(c['entropy_rule']=='off')+int(c['duplicity']==0)+int(c['ks']==1 and c['ks_mode']=='raw'),-c['nreads'])
  if bestkey is None or key>bestkey:best=c;bestkey=key
 if best is None:
  results.append(dict(variant=variant,status='No nonempty rule achieved training precision >= 95%',best_training_precision=explorekey[0] if explorekey else None))
  if variant!='all' or explore is None:continue
  best=explore;variant='all_exploratory_below_target'
 pred=predict(best);result=dict(variant=variant,thresholds=best,train=metrics(pred,train),test=metrics(pred,test))
 result['by_test_scenario']={sc:metrics(pred,test&np.array([t['scenario']==sc for t in truth for a in salix])) for sc in sorted({t['scenario'] for t in truth})}
 # Show tuning-source heterogeneity; these folds were used for selection, not independent validation.
 result['by_train_source']={src:metrics(pred,train&np.array([t['source']==src for t in truth for a in salix])) for src in salix[:4]}
 results.append(result)
base_result=dict(variant='baseline',thresholds=base,train=metrics(predict(base),train),test=metrics(predict(base),test));results.insert(0,base_result)
def genus_metrics(pred,mask):
 z=pred.reshape(-1,6).any(axis=1);target=y.reshape(-1,6).any(axis=1);m=mask.reshape(-1,6)[:,0]
 tp=int(np.sum(z&target&m));fp=int(np.sum(z&~target&m));fn=int(np.sum(~z&target&m))
 return dict(tp=tp,fp=fp,fn=fn,precision=tp/(tp+fp) if tp+fp else None,recall=tp/(tp+fn) if tp+fn else None,false_calls_per_sample=fp/int(m.sum()))
gbest=None;gkey=None
for c in choices:
 m=genus_metrics(predict(c),train)
 if m['precision'] is not None and m['precision']>=.95:
  key=(m['recall'],m['precision'],int(c['entropy_rule']=='off')+int(c['duplicity']==0)+int(c['ks']==1 and c['ks_mode']=='raw'),-c['nreads'])
  if gkey is None or key>gkey:gbest,gkey=c,key
if gbest is not None:results.append(dict(variant='genus_selected',thresholds=gbest,train=genus_metrics(predict(gbest),train),test=genus_metrics(predict(gbest),test)))
(P/'threshold_results.json').write_text(json.dumps(results,indent=2))
with (P/'threshold_search.tsv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=list(frontier[0]),delimiter='\t');w.writeheader();w.writerows(frontier)
with (P/'predictions.tsv').open('w') as f:
 w=csv.writer(f,delimiter='\t');w.writerow(['variant','sample','reference','split','truth','accepted'])
 for r in results:
  if 'thresholds' not in r:continue
  pred=predict(r['thresholds'])
  for i,(s,a) in enumerate(keys):w.writerow([r['variant'],s,a,'train' if train[i] else 'test',int(y[i]),int(pred[i])])
print(json.dumps(results,indent=2))
