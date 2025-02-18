#!/usr/bin/env bash

# copy new files and install test_bufmgr extension

VERSION=17.2
DOWNLOAD_DIR=$HOME
SRC_DIR=${DOWNLOAD_DIR}/postgresql-${VERSION}
ASSIGN_DIR=$HOME/cs3223_assign1

if [ ! -d ${ASSIGN_DIR}/test_bufmgr ]; then
	echo "ERROR: ${ASSIGN_DIR}/test_bufmgr missing!"
	exit 1
fi

chmod u+x *.sh
cp -f diff.sh ${ASSIGN_DIR}
cp -f test_bufmgr.c ${ASSIGN_DIR}/test_bufmgr
cp -fr testresults-yaclock-soln ${ASSIGN_DIR}
if [ -d ${SRC_DIR}/contrib/test_bufmgr ]; then
	\rm -rf ${SRC_DIR}/contrib/test_bufmgr
fi
cp -r ${ASSIGN_DIR}/test_bufmgr ${SRC_DIR}/contrib/
cd ${SRC_DIR}/contrib/test_bufmgr
make && make install

