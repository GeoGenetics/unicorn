PREFIX ?= /usr/local

ifeq ($(origin HTSSRC), undefined)
  HTSINC = -I$(PREFIX)/include
  HTSLIB = -L$(PREFIX)/lib
else
  HTSINC = -I$(HTSSRC)/include
  HTSLIB = -L$(HTSSRC)/lib
endif

CFLAGS+=-Wall -Wextra -Wno-unused-function -pedantic -std=c11 -g -Isrc $(HTSINC)/include -fPIC
KFLAGS=-Wall -Wextra -Wno-unused-function -pedantic -g
SRC=$(wildcard src/libunicorn/*.c)
OBJ=$(SRC:.c=.o)
KSRC=src/klib/kthread.c
KOBJ=src/klib/klib.o
LDFLAGS += $(HTSLIB)
GIT_COMMIT := $(shell git rev-parse --short HEAD)

.PHONY: clean all

%.o:%.c src/version.h
	$(CC) -o $(@) $*.c -g -c $(CFLAGS) $(HTSIPTH)

all: klib libunicorn unicorn

libunicorn: $(OBJ)
	ar rcs $(@).a $(OBJ) $(KOBJ)
	cp src/unicorn.h .

klib:
	$(CC) $(KFLAGS) -c -o $(KOBJ) $(KSRC) -fPIC

unicorn:src/main_unicorn.c $(OBJ) src/version.h
	$(CC) -o $@ $< libunicorn.a -Isrc $(CFLAGS) -fPIE $(LDFLAGS) -pie -lhts -lz -lm -lpthread

src/version.h: src/version.h.in
	sed 's/@GIT_COMMIT@/$(GIT_COMMIT)/' $< > $@

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
	rm -f $(OBJ) libunicorn.a unicorn unicorn.h $(KOBJ) data/out.bam data/out.stats.txt

