
CRAB_ROOT ?= /home/andrew/local/crabby

CONFIG ?= llvm-config

CXXFLAGS = $(shell $(CONFIG) --cxxflags)
LDFLAGS = $(shell $(CONFIG) --ldflags) -L$(CRAB_ROOT)/lib
LLVM_LIBS = $(shell $(CONFIG) --libs)
SYSLIBS = $(shell $(CONFIG) --system-libs)
LIBS = -lclangTooling -lclangFrontend -lclangDriver -lclangSerialization -lclangParse -lclangSema -lclangAnalysis -lclangAST -lclangEdit -lclangLex -lclangCodeGen -lclangBasic -lCrab

all: crabclang

crabclang: crabclang.cpp 
	$(CXX) -o crabclang crabclang.cpp $(CXXFLAGS) -I$(CRAB_ROOT)/include $(LDFLAGS) $(LIBS) $(LLVM_LIBS) $(SYSLIBS)

clean:
	rm crabclang 
