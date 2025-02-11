#!/usr/bin/env bash

# CS3223 Assignment 1
# Script to install PostgreSQL 17.2 on MacOS

source ./settings.sh

VERSION=17.2
ASSIGN_DIR=$HOME/NUS/Y3S2/CS3223/assignments/cs3223_assign1
DOWNLOAD_DIR=$HOME
SRC_DIR=${DOWNLOAD_DIR}/postgresql-${VERSION}
FILE=postgresql-${VERSION}.tar.gz

mkdir -p ${DOWNLOAD_DIR}
mkdir -p ${INSTALL_DIR}
mkdir -p ${PGDATA}

# Download PostgreSQL source files
if [ ! -e ${DOWNLOAD_DIR}/${FILE} ]; then
	cd ${DOWNLOAD_DIR}
	wget https://ftp.postgresql.org/pub/source/v${VERSION}/${FILE}
	tar xvfz ${FILE}
fi


# Install PostgreSQL
cd ${SRC_DIR}
export CFLAGS="-O0"

# To process the Postgres documentation, you need to install additional tools.
# The following 2 lines should work on both Mac Intel and ARM.
# For MacPorts alternatives,
# see: https://www.postgresql.org/docs/current/docguide-toolsets.html#DOCGUIDE-TOOLSETS-INST-MACOS

# Remember to install Homebrew if you haven't!
brew install docbook docbook-xsl libxslt fop
export XML_CATALOG_FILES=$(brew --prefix)/etc/xml/catalog

# You may need to find the path to the icu4c package and add it to PKG_CONFIG_PATH variable
# This package is needed to build Postgres 17.2.
# The following line should work on both Mac Intel and ARM.
# If you still get an error, you may need to change the path slightly,
# see: https://viggy28.dev/article/postgres-v16-icu-installation-issue/

export PKG_CONFIG_PATH=$(brew --prefix icu4c)/lib/pkgconfig/:$PKG_CONFIG_PATH

./configure --prefix=${INSTALL_DIR} --enable-debug  --enable-cassert
make clean && make world && make install 

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

# Create a new database cluster.
if [ -d ${PGDATA} ]; then 
	rm -Rf ${PGDATA}
fi
${INSTALL_DIR}/bin/initdb -D ${PGDATA}


# Update ~/.zshrc
PROFILE=~/.zshrc	# Change to ~/.bash_profile if your default shell is bash
echo >> ${PROFILE}
echo "export PATH=${INSTALL_DIR}/bin:\$PATH" >> ${PROFILE}
echo "export MANPATH=${INSTALL_DIR}/share/man:\$MANPATH" >> ${PROFILE}
echo "export PGDATA=${PGDATA}" >> ${PROFILE}
echo "export PGUSER=$(whoami)" >> ${PROFILE}
echo >> ${PROFILE}

exec zsh  # Restart the shell. Use "source ~/.bash_profile" for bash

