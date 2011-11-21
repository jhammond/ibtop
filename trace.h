#ifndef _TRACE_H_
#define _TRACE_H_
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef DEBUG
#define TRACE ERROR
#else
static inline void TRACE(char *fmt, ...) { }
#endif

#define ERROR(fmt,args...) \
    fprintf(stderr, "%s: "fmt, program_invocation_short_name, ##args)

#define FATAL(fmt,args...) \
  do { \
    ERROR(fmt, ##args);\
    exit(1);\
  } while (0)

#define OOM() FATAL("cannot allocate memory\n")

#endif
