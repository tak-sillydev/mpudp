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

void pdebug_tunrecv(const int seq, const int nread, const uint8_t* buf) {
	pdebug("from tun seq=%d : read %lu bytes\n", seq, nread);
	pdebug("IP header says: packet length = %lu\n", ntohs(((unsigned short*)buf)[1]));
	pdebug("Head 8 bytes of buffer = %02X %02X %02X %02X %02X %02X %02X %02X\n",
		buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]
	);
	return;
}

void pdebug_ethrecv(const int seq, const int nread, const uint8_t* buf, sockaddr_in& addr_from) {
	const uint8_t* payload = buf + sizeof(TUN_HEADER);

	pdebug("from eth seq=%d : read %lu bytes\n", seq, nread);
	pdebug("IP header says: packet length = %lu\n", ntohs(((uint16_t*)payload)[1]));
	pdebug("sender addr: %s, port: %d\n", inet_ntoa(addr_from.sin_addr), ntohs(addr_from.sin_port));

	pdebug("TUN_HEADER = %02X %02X %02X %02X %02X %02X %02X %02X\n",
		buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]
	);
	pdebug("Head 8 bytes of data payload = %02X %02X %02X %02X %02X %02X %02X %02X\n",
		payload[0], payload[1], payload[2], payload[3], payload[4], payload[5], payload[6], payload[7]
	);
	return;
}
