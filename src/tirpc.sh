#!/bin/sh 

OPWD=`pwd`

TIRPC_REPO='git://github.com/linuxbox2/ntirpc.git'
TIRPC_BRANCH_NAME='duplex-7'
TIRPC_COMMIT='69a8003224eb4827d8a819d59d18c85b6c27690e'

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

