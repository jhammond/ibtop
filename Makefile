CPPFLAGS = $(DEBUG) -D_GNU_SOURCE -I/opt/ofed/include
CFLAGS = -Wall -Werror -g
LDFLAGS = -lrt -L/opt/ofed/lib64 -libmad -Wl,-rpath,/opt/ofed/lib64

ibtop: ibtop.o dict.o
