#!/bin/bash

version=$1
cd ..
git archive --format=tar --prefix=unicorn-$version/ HEAD > conda/unicorn.tar
cd src/klib
git archive --format=tar --prefix=unicorn-$version/src/klib/ HEAD > ../../conda/klib.tar
cd ../../conda
tar --concatenate --file=unicorn.tar klib.tar
gzip -c unicorn.tar > v${version}.tar.gz
rm unicorn.tar
rm klib.tar
