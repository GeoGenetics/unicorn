#!/bin/bash

cd ..
git archive --format=tar --prefix=unicorn-2.0.0/ HEAD > conda/unicorn.tar
cd src/klib
git archive --format=tar --prefix=unicorn-2.0.0/src/klib/ HEAD > ../../conda/klib.tar
cd ../../conda
tar --concatenate --file=unicorn.tar klib.tar
gzip -c unicorn.tar > v2.0.0.tar.gz
