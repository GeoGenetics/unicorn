/*
MIT License

Copyright (c) 2025 GeoGenetics

Julian Regalado - julian.perez@sund.ku.dk
                  jregalado@bicu.dev
									https://github.com/7PintsOfCherryGarcia

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#ifndef UNICORN_DAMAGE_H
#define UNICORN_DAMAGE_H

#define _XOPEN_SOURCE 700
#define UNICORN_DAMAGE_OUTPOS 5U

typedef struct taxa {
  uint32_t taxid;
  uint64_t count;
  uint64_t subtree_count;
  float *mmm;
  float CTfreq;
  float GAfreq;
  float A;
  float q;
  float c;
  float phi;
  float Zfit;
  float fitCT0;
  float fitGA0;
  float K5[UNICORN_DAMAGE_OUTPOS];
  float N5[UNICORN_DAMAGE_OUTPOS];
  float K3[UNICORN_DAMAGE_OUTPOS];
  float N3[UNICORN_DAMAGE_OUTPOS];
  float Dx5[UNICORN_DAMAGE_OUTPOS];
  float Dx3[UNICORN_DAMAGE_OUTPOS];
  float nll;
} taxa_t;

KHASHL_MAP_INIT(static,
                damagemap_t,
                damagemap,
                uint32_t,
                taxa_t,
                kh_hash_uint32,
                kh_eq_generic)

uint8_t unicorn_computedamage(damagemap_t *map, const utax_t *utax, uint8_t mmm, uint8_t threads);

#endif
