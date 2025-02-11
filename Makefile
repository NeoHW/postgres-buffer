CPP=gcc
OPTS=-g -Wall
LIBS=-lresolv -ldl -lm

# Modify SRC_DIR as necessary
SRC_DIR=$(HOME)/postgresql-17.2

INCLUDE=-I$(SRC_DIR)/src/include     

freelist_yaclock.o: freelist_yaclock.c
	$(CPP) $(OPTS) $(INCLUDE) -c -o freelist_yaclock.o freelist_yaclock.c

clean:
	rm -f *.o

yaclock: copyyaclock pgsql

clock: copyclock pgsql

copyyaclock:
	cp freelist_yaclock.c $(SRC_DIR)/src/backend/storage/buffer/freelist.c
	cp bufmgr_yaclock.c $(SRC_DIR)/src/backend/storage/buffer/bufmgr.c

copyclock:
	cp freelist.original.c $(SRC_DIR)/src/backend/storage/buffer/freelist.c
	cp bufmgr.original.c $(SRC_DIR)/src/backend/storage/buffer/bufmgr.c

pgsql:
	cd $(SRC_DIR) && make && make install

