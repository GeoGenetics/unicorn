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
```

## Alternative htslib
```
make HTSSRC=/path/to/htslib/
```

# Developers

Go to [src/README.md](https://github.com/GeoGenetics/unicorn/tree/unicorn/src)
