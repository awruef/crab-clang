#!/bin/bash
set -x
set -e 

BASE_DIR=$1
BUILD_DIR=$2
INSTALL_DIR=$3

get_repo () {
  BR=$1
  REPO=$2
  OUTDIR=$3
  if [ -d "$OUTDIR" ]; then
    pushd .
    cd $OUTDIR
    git pull
    git checkout $BR
    git pull 
    popd
  else
    git clone -b $BR $REPO $OUTDIR
  fi
}

get_repo "master" https://github.com/seahorn/crab.git $BASE_DIR

if [ ! -d "$BUILD_DIR" ]; then
  mkdir $BUILD_DIR
fi

pushd .
cd $BUILD_DIR
cmake -DCMAKE_INSTALL_PREFIX=$INSTALL_DIR -DUSE_LDD=ON -DUSE_APRON=ON -DENABLE_TESTS=ON $BASE_DIR
cmake --build . --target ldd && cmake $BASE_DIR
cmake --build . --target apron && cmake $BASE_DIR
cmake --build . --target install
popd

