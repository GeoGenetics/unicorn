# unicorn
## Alignment stat computation and filtering.

# Installing
## Dependencies
> make
> 
> htslib

## Build
```
# 1. Ask for access to either Julian Regalado or Nicola Vogel 
# 2. Get code
git clone https://github.com/GeoGenetics/unicorn.git
cd unicorn
git submodule init
git submodule update
# 3. Compile
make
# 4. Execute 
./unicron
unicorn 0.0.0
./unicorn command [options] -b <in.bam>|<in.sam>|<in.cram>
Commands:
  alnstats    Compute per alingments statitics such as:
                  # alingments, ANI, GC, etc.
  refstats    Compute per reference statistics such as
                  # alignments, # reads, mean read length, etc.
```

## Alternative htslib
If htslib is not in a system-wide path, you can specify manualy with:
```
git clone https://github.com/GeoGenetics/unicorn.git
cd unicorn
git submodule init
git submodule update
make HTSSRC=/path/to/htslib/
./unicorn
```
Make sure `/path/to/htslib` is searchable for loading shared libraries.

# Developers

Go to [src/README.md](https://github.com/GeoGenetics/unicorn/tree/unicorn/src)
