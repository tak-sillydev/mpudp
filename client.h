#include <thread>
#include <memory>

#include "network.h"

namespace client {
	bool getaddress(const char* dst_addr, const int dst_port, addrinfo **result);
	bool setup(SOCKET_PACK& sp, std::string& device_name, const addrinfo* pai);
	std::unique_ptr<std::thread> send_echo_thread(std::vector<SOCKET_PACK>& socks);
	void process_echo_packet(const ECHO_PACKET *echo);
	bool main_loop(int sock_tun, std::vector<SOCKET_PACK>& socks);
}