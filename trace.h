#ifndef _TRACE_H_
#define _TRACE_H_
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

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

#endif
