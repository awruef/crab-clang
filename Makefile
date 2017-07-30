CONFIG ?= llvm-config

CXXFLAGS = $(shell $(CONFIG) --cxxflags)

all: crabclang

crabclang: crabclang.cpp 
	$(CXX) -o crabclang crabclang.cpp $(CXXFLAGS)

clean:
	rm crabclang 
