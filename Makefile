UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  PREFIX ?= $(shell if [ -d /opt/homebrew ]; then echo /opt/homebrew; else echo /usr/local; fi)
else
  PREFIX ?= /usr/local
endif

ifeq ($(origin HTSSRC), undefined)
  HTSINC = -I$(PREFIX)
  HTSLIB = -L$(PREFIX)
else
  HTSINC = -I$(HTSSRC)
  HTSLIB = -L$(HTSSRC)
endif

CFLAGS+=-Wall -Wextra -Wsign-compare -Wno-unused-function -pedantic -std=c11 -Isrc $(HTSINC)/include -Isrc/genesisC -fPIC
ifeq ($(DEBUG),1)
CFLAGS += -g
else
CFLAGS += -O3
endif



KFLAGS=-Wall -Wextra -Wno-unused-function -pedantic -g3 #-fsanitize=address
SRC=$(wildcard src/libunicorn/*.c)
OBJ=$(SRC:.c=.o)
KSRC=src/klib/kthread.c
KOBJ=src/klib/klib.o
LDFLAGS += $(HTSLIB)/lib
GIT_COMMIT := $(shell git rev-parse --short HEAD)
GENESIS ?= src/genesis

# Use genesis submodule if present, otherwise fall back to conda installation
GENESIS_SUBMODULE_PRESENT := $(shell [ -f $(GENESIS)/CMakeLists.txt ] && echo yes || echo no)
ifeq ($(GENESIS_SUBMODULE_PRESENT),yes)
  GENESIS_PREFIX_FOR_BUILD := $(CURDIR)/$(GENESIS)
  USE_CONDA_GENESIS := no
else
  GENESIS_PREFIX_FOR_BUILD := $(CONDA_PREFIX)
  USE_CONDA_GENESIS := yes
endif

.PHONY: clean all

%.o:%.c src/version.h
	$(CC) -o $(@) $*.c -c $(CFLAGS) $(HTSIPTH)

all: genesis genesisC klib libunicorn unicorn

libunicorn: $(OBJ)
	ar rcs $(@).a $(OBJ) $(KOBJ)
	cp src/unicorn.h .

klib:
	$(CC) $(KFLAGS) -c -o $(KOBJ) $(KSRC) -fPIC

genesis:
ifeq ($(USE_CONDA_GENESIS),yes)
	@echo "Using conda-installed genesis from $(CONDA_PREFIX)"
else
	$(MAKE) -C src/genesis
endif

genesisC: genesis
	$(MAKE) -C src/genesisC GENESIS_PREFIX=$(GENESIS_PREFIX_FOR_BUILD) genesisC

unicorn: src/main_unicorn.c $(OBJ) src/version.h
	$(CC) -o $@ $< libunicorn.a src/genesisC/genesisC.o -Isrc $(CFLAGS) -fPIE $(LDFLAGS) -lhts -lz -lm -lpthread -lstdc++

src/version.h: src/version.h.in
	sed 's/@GIT_COMMIT@/$(GIT_COMMIT)/' $< > $@

test:
	cksum=$$(./unicorn refstats -b data/test.bam 2> /dev/null | cksum | awk '{print $$1}' ); \
	[ $$cksum -eq 996842798 ] || (exit 1)

clean:
	rm -f $(OBJ) src/version.h libunicorn.a unicorn unicorn.h $(KOBJ) data/out.bam data/out.stats.txt

