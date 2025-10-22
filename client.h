#include "network.h"

namespace client {
	bool getaddress(const char* dst_addr, const int dst_port, addrinfo **result);
	bool setup(SOCKET_PACK& sp, std::string& device_name, const addrinfo* pai);
	bool main_loop(int sock_tun, std::vector<SOCKET_PACK>& socks);
}