#!/bin/bash
set -x
set -e 

BRANCH=$1
BASE_DIR=$2
BUILD_DIR=$3
INSTALL_DIR=$4

get_repo () {
  BR=$1
  REPO=$2
  OUTDIR=$3
  if [ -d "$OUTDIR"/.git ]; then
    git pull 
  else
    git clone -b $BR $REPO $OUTDIR
  fi
}

get_repo $BRANCH https://github.com/llvm-mirror/llvm.git $BASE_DIR
get_repo $BRANCH https://github.com/llvm-mirror/clang.git $BASE_DIR/tools/clang
#get_repo $BRANCH https://github.com/llvm-mirror/polly.git $BASE_DIR/tools/polly
#get_repo $BRANCH https://github.com/llvm-mirror/clang-tools-extra.git $BASE_DIR/tools/clang/tools/extra
#get_repo $BRANCH https://github.com/llvm-mirror/compiler-rt.git $BASE_DIR/runtimes/compiler-rt
#get_repo $BRANCH https://github.com/llvm-mirror/libcxx.git $BASE_DIR/projects/libcxx
#get_repo $BRANCH https://github.com/llvm-mirror/libcxxabi.git $BASE_DIR/projects/libcxxabi
#get_repo $BRANCH https://github.com/llvm-mirror/libunwind.git $BASE_DIR/projects/libunwind

if [ ! -d "$BUILD_DIR" ]; then
  mkdir $BUILD_DIR
fi
  pushd .
  cd $BUILD_DIR
  cmake -G Ninja -DLLVM_ENABLE_RTTI=1 -DLLVM_ENABLE_EH=1 -DLLVM_TARGETS_TO_BUILD=X86 -DCMAKE_INSTALL_PREFIX=$INSTALL_DIR -DLLVM_ENABLE_LTO=OFF -DLLVM_PARALLEL_LINK_JOBS=1 $BASE_DIR
  ninja all
  ninja install
  popd
