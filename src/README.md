# unicorn development

This repository contains code for the [unicorn](https://github.com/GeoGenetics/unicorn/blob/unicorn/src/main_unicorn.c) command line program:

`./unicorn`
```
unicorn 0.0.0
./unicorn command [options] -b <in.bam>|<in.sam>|<in.cram>
Commands:
  alnstats    Compute per alingments statitics such as:
                  # alingments, ANI, GC, etc.
  refstats    Compute per reference statistics such as
                  # alignments, # reads, mean read length, etc.
```

As well as for the unicorn library `libunicorn` which contains the project's functionality via
unicorn's API: [unicorn.h](https://github.com/GeoGenetics/unicorn/blob/unicorn/src/unicorn.h)

## Building libunicorn

### Requirements

>make

>[htslib](https://github.com/samtools/htslib/tree/develop) >= 1.21

Running `make` in the base directory will build the entire porject into:

```
unicorn      //Command line program
libunicorn.a //Static library
unicorn.h    //API
```

You can supply a costum path to an alternative htslib instalation with `HTSSRC`. For example:

Assuming htslib is installed in:

`/path/to/alternative/htslibinstall`

```
$ ls /path/to/alternative/htslibinstall
   lib
   include 
```

`make HTSSRC=/path/to/alternative/htslibinstall`

Will compile the entire project using the provided htslib instalation.

## Linking your project to libunicorn

Just add `unicorn.h` to your program's source and compile with `libunicorn.a`

```
//test_unicorn.c
//gcc -Wall -Wextra -pedantic -std=c11 -o test_unicorn test_unicorn.c libunicorn.a -lhts
#include <stdio.h>
#include "unicorn.h"
int main()
{
  fprintf(stderr, "Using unicorn version: ", unicorn_version());
  return 0;
}

gcc -Wall -Wextra -pedantic -std=c11 -o test_unicorn test_unicorn.c libunicorn.a -lhts

$ ./test_unicorn
Using unicorn version: 0.0.0
```

# Contribute

Bugs and requests are handled via github [issues](https://github.com/GeoGenetics/unicorn/issues)

In order to contribute code, please refer to [DEVELOP.md](https://github.com/GeoGenetics/unicorn/blob/unicorn/src/DEVELOP.md)
