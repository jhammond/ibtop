CPPFLAGS = -DDEBUG -D_GNU_SOURCE -I/opt/ofed/include
CFLAGS = -Wall -Werror -g
LDFLAGS = -lrt -L/opt/ofed/lib64 -libmad -Wl,-rpath,/opt/ofed/lib64 /usr/lib64/libgdbm.so.2

all: ibtop umad-pq umad-pma parse-current-net

ibtop: ibtop.o ib-net-db.o

# ibtop: ibtop.c
# ibtop-pma: ibtop-pma.c
# parse-current-net: parse-current-net.c
