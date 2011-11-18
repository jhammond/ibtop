CPPFLAGS = -DDEBUG -D_GNU_SOURCE -I/opt/ofed/include
CFLAGS = -Wall -Werror -g
LDFLAGS = -L/opt/ofed/lib64 -libmad -Wl,-rpath,/opt/ofed/lib64

all: ibtop ibtop-pma parse-current-net

# ibtop: ibtop.c
# ibtop-pma: ibtop-pma.c
# parse-current-net: parse-current-net.c
