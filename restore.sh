#!/usr/bin/env bash

# CS3223 Assignment 1 
# Script to restore PostgreSQL installation 

source ./settings.sh

pg_ctl stop

\rm -rf  ${PGDATA}
mkdir -p ${PGDATA}

make clock

# Create a database cluster
${INSTALL_DIR}/bin/initdb -D ${PGDATA}

