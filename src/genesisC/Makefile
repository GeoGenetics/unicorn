ifndef GENESIS
$(error GENESIS variable not set. Invoke as: make GENESIS=/path/to/genesis)
endif

CXX := g++
CC  := gcc

CXXFLAGS := -I$(GENESIS)/lib/ -I$(GENESIS)/build/generated/include/ -O3
CFLAGS   := -Wall -Wextra -pedantic -O3
LDFLAGS  := -L$(GENESIS)/bin/ -Wl,--rpath,$(GENESIS)/bin
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
