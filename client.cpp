#include "print.h"
#include "client.h"

namespace client {
	bool getaddress(const char* dst_addr, const int dst_port, addrinfo **result) {
		char		port_str[16];
		addrinfo	hints;
		int			err;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family		= AF_INET;
		hints.ai_socktype	= SOCK_DGRAM;

		sprintf(port_str, "%hu", dst_port);
		if ((err = getaddrinfo(dst_addr, port_str, &hints, result)) < 0) {
			perror("getaddrinfo()");
			print_error("err = %d, reason = %s\n", err, gai_strerror(err));
			print_error("errno = %d\n", errno);
			return false;
		}
		return true;
	}

	bool setup(SOCKET_PACK& s, std::string& device_name, const addrinfo* pai, int& max_fd) {
		// 使用する実デバイスにソケットを割り当てる
		// ソケットの作成とオプションの設定
		socklen_t	szaddr = sizeof(sockaddr_in);
		int			optval = 1;

		if ((s.sock_fd = socket(pai->ai_family, pai->ai_socktype, pai->ai_protocol)) < 0) {
			perror("socket()");
			print_error("Couldn't create socket - %s\n", device_name.c_str());
			print_error("errno = %d\n", errno);
			return false;
		}
		if (setsockopt(s.sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_REUSEASSR - %s\n", device_name.c_str());
			print_error("errno = %d\n", errno);
			return false;
		}
		if (setsockopt(s.sock_fd, SOL_SOCKET, SO_BINDTODEVICE, device_name.c_str(), device_name.length()) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_BINDTODEVICE - %s\n", device_name.c_str());
			print_error("errno = %d\n", errno);
			return false;
		}

		// 接続先の設定
		if (connect(s.sock_fd, pai->ai_addr, pai->ai_addrlen) < 0) {
			perror("connect()");
			print_error("device = %s, errno = %d\n", s.eth_name.c_str(), errno);
			return false;
		}
		s.remote_addr = *(sockaddr_in *)pai->ai_addr;

		// ローカル側で使用するポートの割当
		s.local_addr.sin_family			= AF_INET;
		s.local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
		s.local_addr.sin_port			= htons(0);
		bind(s.sock_fd, (sockaddr*)&(s.local_addr), sizeof(sockaddr));
		getsockname(s.sock_fd, (sockaddr*)&(s.local_addr), &szaddr);	// bind() によって使用ポートが割り当てられたので情報を取得

		s.eth_name = device_name;

		pdebug("eth[%s]: fd: %d, local addr: %s, port: %d\n",
			s.eth_name.c_str(),
			s.sock_fd,
			inet_ntoa(s.local_addr.sin_addr),
			ntohs(s.local_addr.sin_port)
		);
		max_fd = max(s.sock_fd, max_fd);
		return true;
	}

	bool main_loop() {
		return true;
	}
} // namespace client
