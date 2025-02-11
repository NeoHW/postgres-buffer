#!/usr/bin/env bash

# CS3223 Assignment 1
# Script to install PostgreSQL 17.2

source ./settings.sh

VERSION=17.2
ASSIGN_DIR=$HOME/cs3223_assign1
DOWNLOAD_DIR=$HOME
SRC_DIR=${DOWNLOAD_DIR}/postgresql-${VERSION}
FILE=postgresql-${VERSION}.tar.gz

mkdir -p ${DOWNLOAD_DIR}
mkdir -p ${INSTALL_DIR}
mkdir -p ${PGDATA}

# Download PostgreSQL source files
if [ ! -e ${DOWNLOAD_DIR}/${FILE} ]; then
	cd ${DOWNLOAD_DIR}
	wget https://ftp.postgresql.org/pub/source/v${VERSION}/$FILE
	tar xvfz ${FILE}
fi

if [ ! -d ${SRC_DIR} ]; then
	cd ${DOWNLOAD_DIR}
	tar xvfz ${FILE}
fi

# Install PostgreSQL
cd ${SRC_DIR}
export CFLAGS="-g"
./configure --prefix=${INSTALL_DIR} --enable-debug  --enable-cassert
NUMBER_PARALLEL_TASKS=2
make clean && make world-bin && make install -j ${NUMBER_PARALLEL_TASKS}

# Install test_bufmgr extension
if [ ! -d ${ASSIGN_DIR} ]; then
	echo "Error: Assignment directory ${ASSIGN_DIR} missing!"
	exit 1
fi
if [ ! -d ${SRC_DIR}/contrib/test_bufmgr ]; then
	cp -r ${ASSIGN_DIR}/test_bufmgr ${SRC_DIR}/contrib
	cd ${SRC_DIR}/contrib/test_bufmgr
	make && make install 
fi
chmod u+x ${ASSIGN_DIR}/*.sh

# Create a database cluster
if [ -d ${PGDATA} ]; then 
	rm -Rf ${PGDATA}
fi
${INSTALL_DIR}/bin/initdb -D ${PGDATA}


