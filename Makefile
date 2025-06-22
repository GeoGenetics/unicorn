CPP=g++

ifndef HTSSRC
$(info HTSSRC not defined; expecting systemwide htslib instalation)
else
HTSIPTH=-I"$(realpath $(HTSSRC)/include)"
HTSLPTH=-L"$(realpath $(HTSSRC)/lib)"
$(info htslib include dir is $(HTSIPTH))
$(info htslib lib dir is $(HTSLPTH))
endif

.PHONY:all

unicron:src/main_unicorn.cpp
	$(CPP) -o $@ $< $(HTSIPTH) $(HTSLPTH) -Wall -Wextra -pedantic -std=c++11 -g -lhts

all: unicorn
	
