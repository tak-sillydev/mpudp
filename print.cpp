#include "print.h"
#include "network.h"

void print_error(const char *format, ...) {
	va_list	arg;

	va_start(arg, format);
	vfprintf(stderr, format, arg);
	va_end(arg);

	return;
}

void print_debug(const char *format, ...) {
	va_list	arg;

	va_start(arg, format);
	vfprintf(stderr, format, arg);
	va_end(arg);

	return;
}

void pdebug_tunrecv(const int seq, const int nread, const char* buf) {
	pdebug("from tun seq=%d : read %lu bytes\n", seq, nread);
	pdebug("IP header says: packet length = %lu\n", ntohs(((unsigned short*)buf)[1]));
	pdebug("Head 8 bytes of buffer = %02X %02X %02X %02X %02X %02X %02X %02X\n",
		(unsigned char)buf[0], (unsigned char)buf[1], (unsigned char)buf[2], (unsigned char)buf[3],
		(unsigned char)buf[4], (unsigned char)buf[5], (unsigned char)buf[6], (unsigned char)buf[7]
	);
	return;
}

void pdebug_ethrecv(const int seq, const int nread, const char* buf, sockaddr_in& addr_from) {
	const char* payload = buf + sizeof(TUN_HEADER);

	pdebug("from eth seq=%d : read %lu bytes\n", seq, nread);
	pdebug("IP header says: packet length = %lu\n", ntohs(((unsigned short*)payload)[1]));
	pdebug("sender addr: %s, port: %d\n", inet_ntoa(addr_from.sin_addr), ntohs(addr_from.sin_port));

	pdebug("TUN_HEADER = %02X %02X %02X %02X %02X %02X %02X %02X\n",
		((unsigned char*)buf)[0], ((unsigned char*)buf)[1], ((unsigned char*)buf)[2], ((unsigned char*)buf)[3],
		((unsigned char*)buf)[4], ((unsigned char*)buf)[5], ((unsigned char*)buf)[6], ((unsigned char*)buf)[7]
	);
	pdebug("Head 8 bytes of data payload = %02X %02X %02X %02X %02X %02X %02X %02X\n",
		(unsigned char)payload[0], (unsigned char)payload[1], (unsigned char)payload[2], (unsigned char)payload[3],
		(unsigned char)payload[4], (unsigned char)payload[5], (unsigned char)payload[6], (unsigned char)payload[7]
	);
	return;
}
