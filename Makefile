CPPFLAGS = -D_GNU_SOURCE -I/opt/ofed/include
CFLAGS = -Wall -Werror -g
LDFLAGS = -L/opt/ofed/lib64 -libmad

ibtop: ibtop.c
