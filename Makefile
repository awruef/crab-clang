
CRAB_ROOT ?= /home/andrew/local/crab
CONFIG ?= llvm-config
APRON_ROOT ?= /home/andrew/crab/build/run/apron

CXXFLAGS = $(shell $(CONFIG) --cxxflags) -g -ggdb -DHAS_APRON
LDFLAGS = $(shell $(CONFIG) --ldflags) -L$(CRAB_ROOT)/lib -L$(APRON_ROOT)/lib
LLVM_LIBS = $(shell $(CONFIG) --libs)
SYSLIBS = $(shell $(CONFIG) --system-libs)
LIBS = -lclangTooling -lclangFrontend -lclangRewrite -lclangRewriteFrontend -lclangDriver -lclangSerialization -lclangParse -lclangSema -lclangAnalysis -lclangAST -lclangEdit -lclangLex -lclangCodeGen -lclangBasic -lCrab -lpolkaMPQ -loctD -lapron -lgmp -lmpfr

all: crabclang

crabclang: crabclang.cpp 
	$(CXX) -o crabclang crabclang.cpp $(CXXFLAGS) -I$(APRON_ROOT)/include -I$(CRAB_ROOT)/include $(LDFLAGS) $(LIBS) $(LLVM_LIBS) $(SYSLIBS)

clean:
	rm crabclang 
