CPPFLAGS = $(DEBUG) -D_GNU_SOURCE -I/opt/ofed/include
CFLAGS = -Wall -Werror -g
LDFLAGS = -lrt -L/opt/ofed/lib64 -libmad -Wl,-rpath,/opt/ofed/lib64 /usr/lib64/libgdbm.so.2

all: ibtop ibpq make-ib-net-db # umad-pq umad-pma parse-current-net

ibtop: ibtop.o ib-net-db.o dict.o
ibpq: ibpq.o ib-net-db.o
make-ib-net-db: make-ib-net-db.o ib-net-db.o

# ibtop: ibtop.c
# ibtop-pma: ibtop-pma.c
# parse-current-net: parse-current-net.c
