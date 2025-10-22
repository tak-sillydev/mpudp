#ifndef __PRINT_H__
#define	__PRINT_H__

#include <stdio.h>
#include <stdarg.h>
#include "network.h"

void print_error(const char *format, ...);
void print_debug(const char *format, ...);

void pdebug_tunrecv(const int seq, const int nread, const uint8_t* buf);
void pdebug_ethrecv(const int seq, const int nread, const uint8_t* buf, sockaddr_in& addr_from);

#define pdebug(format, ...)	{if(_global_fDebug){print_debug((format),##__VA_ARGS__);}}

extern bool _global_fDebug;
#endif