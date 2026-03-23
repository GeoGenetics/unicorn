GENESIS_PREFIX ?= $(CONDA_PREFIX)

CXX := g++
CC  := gcc

# If GENESIS_PREFIX points to a source tree (has lib/genesis/genesis.hpp),
# use the source layout; otherwise use the installed conda layout.
GENESIS_IS_SOURCETREE := $(shell [ -f $(GENESIS_PREFIX)/lib/genesis/genesis.hpp ] && echo yes || echo no)
ifeq ($(GENESIS_IS_SOURCETREE),yes)
  GENESIS_INC := -I$(GENESIS_PREFIX)/lib/ -I$(GENESIS_PREFIX)/build/generated/include/
  GENESIS_LIB := -L$(GENESIS_PREFIX)/bin/ -Wl,-rpath,$(GENESIS_PREFIX)/bin
else
  GENESIS_INC := -I$(GENESIS_PREFIX)/include/
  GENESIS_LIB := -L$(GENESIS_PREFIX)/lib/ -Wl,-rpath,$(GENESIS_PREFIX)/lib
endif

CXXFLAGS := $(GENESIS_INC) -O3
CFLAGS   := -Wall -Wextra -pedantic -O3
LDFLAGS  := $(GENESIS_LIB)
LDLIBS   := -lgenesis -lstdc++ -lz

SRC_C   := camex_aln.c
SRC_CPP := genesisC.cpp
OBJ     := genesisC.o
TARGET  := camex_aln

.PHONY: all clean genesisC

all: $(TARGET)

$(OBJ): $(SRC_CPP)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(TARGET): $(OBJ) $(SRC_C)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRC_C) $(OBJ) $(LDLIBS)

genesisC: $(OBJ)

clean:
	rm -f $(TARGET) $(OBJ)
