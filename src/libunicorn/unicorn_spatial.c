#include "unicorn_internal.h"

#define LINEAR_LT(a,b) ((a).linear < (b).linear)
#define CIRCULAR_LT(a,b) ((a).circular < (b).circular)
KSORT_INIT(spatial_linear, spatial_pos_t, LINEAR_LT)
KSORT_INIT(spatial_circular, spatial_pos_t, CIRCULAR_LT)

int unicorn_spatial_add(spatial_t *s, int64_t start, int64_t end, uint64_t length)
{
  /* Exclude undefined linear coordinates from all three distances so their
   * denominators agree. Full-length spans have no spatial freedom. */
  if (!length || start < 0 || end <= start || (uint64_t)end > length ||
      (uint64_t)(end-start) >= length) { ++s->invalid; return 0; }
  spatialq_t *v = &s->positions;
  if (v->n == v->m) {
    size_t next = v->m ? v->m*2 : 2;
    if (next < v->m || next > SIZE_MAX/sizeof(*v->a)) return -1;
    void *a = unicorn_mem_resize(&unicorn_spill_threshold,v->a,next*sizeof(*v->a));
    if (!a) return -1;
    v->a = a; v->m = next;
  }
  v->a[v->n++] = (spatial_pos_t){(double)start/(length-(end-start)),(double)start/length};
  return 0;
}
void unicorn_spatial_free(spatial_t *s)
{
  unicorn_mem_free(&unicorn_spill_threshold,s->positions.a);
  s->positions = (spatialq_t){0};
}
void unicorn_spatial_finish(spatial_t *s)
{
  spatialq_t *v=&s->positions;
  if (!v->n) { s->ks=s->kuiper=s->circular=NAN; return; }
  s->valid=v->n;
  ks_introsort(spatial_linear,v->n,v->a);
  double plus=0,minus=0,n=(double)v->n;
  for(size_t i=0;i<v->n;++i) {
    double u=v->a[i].linear;
    plus=fmax(plus,(i+1)/n-u); minus=fmax(minus,u-i/n);
  }
  s->ks=fmax(plus,minus); s->kuiper=plus+minus;
  ks_introsort(spatial_circular,v->n,v->a);
  plus=minus=0;
  for(size_t i=0;i<v->n;++i) {
    double u=v->a[i].circular;
    plus=fmax(plus,(i+1)/n-u); minus=fmax(minus,u-i/n);
  }
  s->circular=plus+minus;
  unicorn_spatial_free(s);
}
