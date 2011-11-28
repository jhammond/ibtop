VERSION = 1.0.0
BINDIR = /usr/local/bin
CPPFLAGS = $(DEBUG) -D_GNU_SOURCE -DBINDIR=\"$(BINDIR)\" -DVERSION=\"$(VERSION)\" -I/opt/ofed/include 
CFLAGS = -Wall -Werror -g
LDFLAGS = -lrt -L/opt/ofed/lib64 -libmad -Wl,-rpath,/opt/ofed/lib64

all: ibtop make-net-info

ibtop: ibtop.o dict.o

make-net-info: make-net-info.o

.PHONY: clean
clean:
	rm -f ibtop make-net-info *.o
