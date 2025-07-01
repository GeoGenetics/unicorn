CPP=g++
CC=gcc
CFLAGS=-Wall -Wextra -Wno-unused-function -pedantic -std=c11 -g -Isrc
SRC=$(wildcard src/libunicorn/*.c)
OBJ=$(SRC:.c=.o)
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
	ar rcs $(@).a $(OBJ)
	cp src/unicorn.h .

unicorn: src/main_unicorn.c $(OBJ)
	$(CC) -o $@ $< libunicorn.a $(HTSIPTH) $(HTSLPTH) -Isrc $(CFLAGS) -lhts -lm

clean:
	rm -f $(OBJ) libunicorn.a unicorn

