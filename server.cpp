#include "print.h"
#include "server.h"

namespace server {
	bool setup(int& sock_recv, const int stby_port) {
		sockaddr_in	local_addr;
		socklen_t	szaddr = sizeof(sockaddr_in);
		int			optval = 1;

		// ソケットの作成とオプションの設定
		if ((sock_recv = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			perror("socket()");
			print_error("Couldn't create socket\n");
			print_error("errno = %d\n", errno);
			return false;
		}
		if (setsockopt(sock_recv, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_REUSEASSR\n");
			print_error("errno = %d\n", errno);
			return false;
		}
		memset(&local_addr, 0, sizeof(local_addr));
		local_addr.sin_family		= AF_INET;
		local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
		local_addr.sin_port			= htons(stby_port);

		if (bind(sock_recv, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
			perror("bind()");
			print_error("errno = %d\n", errno);
			return false;
		}
		getsockname(sock_recv, (sockaddr*)&local_addr, &szaddr);
		pdebug("eth: addr: %s, port: %d\n",
			inet_ntoa(local_addr.sin_addr),
			ntohs(local_addr.sin_port)
		);
		return true;
	}
} // namespace server 
