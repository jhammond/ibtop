CPPFLAGS = $(DEBUG) -D_GNU_SOURCE -I/opt/ofed/include
CFLAGS = -Wall -Werror -g
LDFLAGS = -lrt -L/opt/ofed/lib64 -libmad -Wl,-rpath,/opt/ofed/lib64

all: ibtop make-net-info

ibtop: ibtop.o dict.o

make-net-info: make-net-info.o
