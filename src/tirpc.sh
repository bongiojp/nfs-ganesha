#!/bin/sh 

OPWD=`pwd`

TIRPC_REPO='https://github.com/bongiojp/libtirpc-lbx.git'
TIRPC_BRANCH_NAME='duplex-7'
TIRPC_COMMIT='f57314f75c2c6aa2fe4ec8b84fe2579915f5882c'

# remove libtirpc if present;  try to avoid making
# a mess
if [ -d ../src -a -d ../contrib ]; then
    if [ -e libtirpc ]; then
	rm -rf libtirpc
    fi
fi

git clone ${TIRPC_REPO} libtirpc
cd libtirpc
git checkout -b $TIRPC_BRANCH_NAME ${TIRPC_COMMIT}
cd ${OPWD}

./autogen.sh

