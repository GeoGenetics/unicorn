![C/C++ CI](https://github.com/GeoGenetics/unicorn/actions/workflows/c-cpp.yml/badge.svg?branch=unicorn)
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
```

### Alternative htslib

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

## running unicorn
```
./unicron
unicorn 0.0.0
./unicorn command [options] -b <in.bam>|<in.sam>|<in.cram>
Commands:
  refstats    Compute per reference statistics such as
              # alignments, # reads, mean read length, etc.
```

`refstats` will print the statistics as a tab-separated text file.

## Statistics

```
1 - Id             reference name
2 - Length        reference length
3 - n_alns        number of alignments to the reference
4 - n_reads       number of reads to the reference always < n_alns
5 - m_readl       median read length
6 - std_readl     standard deviation of read length
7 - md_readl      mode of read length
8 - readl_min     smallest read length
9 - readl_max     largest read length
10 - m_alnnm      mean alignment edit distance
11 - m_alnani     mean alignment ani
12 - std_alnani   standard deviation of alignment ani
13 - md_alnani    mode of alignment ani
```



# Developers

Go to [src/README.md](https://github.com/GeoGenetics/unicorn/tree/unicorn/src)
