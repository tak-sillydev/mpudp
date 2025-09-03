#include "print.h"

#define	DEBUG

void print_error(const char *format, ...) {
	va_list	arg;

	va_start(arg, format);
	vfprintf(stderr, format, arg);
	va_end(arg);
}

void print_debug(const char *format, ...) {
#ifdef DEBUG
	va_list	arg;

	va_start(arg, format);
	vfprintf(stderr, format, arg);
	va_end(arg);
#endif
}