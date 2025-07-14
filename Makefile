CPP=g++
CC=gcc
CFLAGS=-Wall -Wextra -Wno-unused-function -pedantic -std=c11 -g -Isrc
KFLAGS=-Wall -Wextra -Wno-unused-function -pedantic -g
SRC=$(wildcard src/libunicorn/*.c)
OBJ=$(SRC:.c=.o)
KSRC=src/klib/kthread.c
KOBJ=src/klib/klib.o

ifndef HTSSRC
$(info HTSSRC not defined; expecting systemwide htslib instalation)
else
HTSIPTH=-I"$(realpath $(HTSSRC)/include)"
HTSLPTH=-L"$(realpath $(HTSSRC)/lib)"
$(info htslib include dir is $(HTSIPTH))
$(info htslib lib dir is $(HTSLPTH))
endif

.PHONY: clean all

%.o:%.c
	$(CC) -o $(@) $*.c -g -c $(CFLAGS) $(HTSIPTH)

all: libunicorn unicorn

libunicorn: $(OBJ)
	ar rcs $(@).a $(OBJ) $(KOBJ)
	cp src/unicorn.h .

klib:
	$(CC) $(KFLAGS) -c -o $(KOBJ) $(KSRC)

unicorn: src/main_unicorn.c $(OBJ) klib
	$(CC) -o $@ $< libunicorn.a $(HTSIPTH) $(HTSLPTH) -Isrc $(CFLAGS) -lhts -lm

test:
	./unicorn refstats -b data/test.bam -o data/out
	cksum=$$(cksum data/out.bam  | cut -f1 -d ' '); \
	[ $$cksum -eq 3158363077 ] || (exit 1)
	cksum=$$(cksum data/out.stats.txt  | cut -f1 -d ' '); \
	[ $$cksum -eq 84514074 ] || (exit 1)
	./unicorn bamstats -b data/test.bam -o data/out
	cksum=$$(cksum data/out.stats.txt  | cut -f1 -d ' '); \
	[ $$cksum -eq 3397468444 ] || (exit 1)
	rm data/out.bam data/out.stats.txt

clean:
	rm -f $(OBJ) libunicorn.a unicorn

