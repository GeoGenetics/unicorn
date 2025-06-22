CPP=g++
CC=gcc

ifndef HTSSRC
$(info HTSSRC not defined; expecting systemwide htslib instalation)
else
HTSIPTH=-I"$(realpath $(HTSSRC)/include)"
HTSLPTH=-L"$(realpath $(HTSSRC)/lib)"
$(info htslib include dir is $(HTSIPTH))
$(info htslib lib dir is $(HTSLPTH))
endif

.PHONY:all

_unicron: src/main_unicorn.c
	$(CC) -o unicorn $< $(HTSIPTH) $(HTSLPTH) -Wall -Wextra -pedantic -std=c11 -g -lhts

unicorncpp: src/main_unicorn.cpp
	$(CPP) -o $@ $< $(HTSIPTH) $(HTSLPTH) -Wall -Wextra -pedantic -std=c++11 -g -lhts

unicorn:unicorncpp _unicron
	
all: unicorn
