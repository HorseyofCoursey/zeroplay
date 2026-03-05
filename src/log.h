#ifndef LOG_H
#define LOG_H

#include <stdio.h>

extern int g_verbose;

/* vlog() — prints only when -v is passed */
#define vlog(...) do { if (g_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

#endif
