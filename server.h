#include "network.h"

namespace server {
	bool setup(int& sock_recv, const int stby_port);
	bool main_loop(int sock_tun, int sock_recv, std::vector<SOCKET_PACK>& socks);
}