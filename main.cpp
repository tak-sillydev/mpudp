#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>

int main(void) {
	int	sock;
	sockaddr_in	addr, recv_addr;
	unsigned int	szrecvaddr = sizeof(recv_addr);
	char	buf[2048];

	ssize_t	szrecv;
	int	ip0, ip1, ip2, ip3;

	sock = socket(AF_INET, SOCK_DGRAM, 0);

	if (sock == -1) { printf("FATAL: create socket\n"); return 0; }

	addr.sin_family			= AF_INET;
	addr.sin_addr.s_addr	= INADDR_ANY;
	addr.sin_port			= htons(2683);
	if (bind(sock, (sockaddr *)&addr, sizeof(addr)) == -1) { printf("FATAL: bind socket\n"); return 0; }

	while (true) {
		szrecv = recvfrom(sock, buf, 2048, MSG_WAITALL, (sockaddr *)&recv_addr, &szrecvaddr);
		ip0	= (recv_addr.sin_addr.s_addr >>  0) & 0xff;
		ip1	= (recv_addr.sin_addr.s_addr >>  8) & 0xff;
		ip2	= (recv_addr.sin_addr.s_addr >> 16) & 0xff;
		ip3	= (recv_addr.sin_addr.s_addr >> 24) & 0xff;
		printf("%ld bytes from %d.%d.%d.%d:%d\n", szrecv, ip0, ip1, ip2, ip3, recv_addr.sin_port);
	}
	printf("No problems occured\n");
	return 0;
}