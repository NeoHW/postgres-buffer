#!/bin/bash

export PGPORT=5075
export PGUSER=$(whoami)
export PGDATA=/tmp/${PGUSER}/pgdata-cs3223
ASSIGN_DIR=$HOME/cs3223_assign1
INSTALL_DIR=$HOME/pgsql-cs3223
LOG_FILE=${ASSIGN_DIR}/log.txt
DBNAME=assign1

BASH_PROFILE=$HOME/.bash_profile
touch ${BASH_PROFILE} 
cat <<EOF >> ${BASH_PROFILE}

export PATH=${INSTALL_DIR}/bin:$PATH
export MANPATH=${INSTALL_DIR}/share/man:$MANPATH
export PGDATA=${PGDATA}
export PGUSER=${PGUSER}
export PGPORT=${PGPORT}
EOF
source ${BASH_PROFILE}
