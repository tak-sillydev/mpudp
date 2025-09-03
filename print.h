#ifndef __PRINT_H__

#include <stdio.h>
#include <stdarg.h>

void print_error(const char *format, ...);
void print_debug(const char *format, ...);

#define	__PRINT_H__
#endif