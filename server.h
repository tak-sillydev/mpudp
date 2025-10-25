#include "network.h"

namespace server {
	bool setup(int& sock_recv, const int stby_port);
	ssize_t send_stable(uint8_t *buf, int sock_recv, std::vector<SOCKET_PACK>& socks, uint16_t data_len);
	bool main_loop(int sock_tun, int sock_recv, std::vector<SOCKET_PACK>& socks);
}