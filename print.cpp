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

const char* const mode2str(uint8_t mode) {
	switch (mode) {
	case MODE_SPEED:  return "MODE_SPEED";
	case MODE_STABLE: return "MODE_STABLE";
	default: return "?";
	}
	return "?";
}

void pdebug_ethrecv(const int seq, const int nread, const uint8_t* buf, sockaddr_in& addr_from) {
	const TUN_HEADER* hdr = (TUN_HEADER*)buf;

	pdebug("from eth seq=%d : read %lu bytes\n", seq, nread);
	pdebug("sender addr: %s, port: %d\n", inet_ntoa(addr_from.sin_addr), ntohs(addr_from.sin_port));

	pdebug(
		"TUN_HEADER info:\n"
		"  mode = %s\n"
		"  device_id = %d\n"
		"  length = %d\n"
		"  seq_all = %d\n"
		"  seq_dev = %d\n",
		mode2str(hdr->mode), hdr->device_id, hdr->length, hdr->seq_all, hdr->seq_dev
	);
	return;
}
