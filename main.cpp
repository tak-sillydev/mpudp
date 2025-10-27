/*
	試作品：IPv4 only 2024-09-07
*/
#include <unistd.h>
#include <errno.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "print.h"
#include "network.h"
#include "ringbuf.h"

#include "mpudp.h"

using namespace std;


#define	MODE_SERVER	0
#define	MODE_CLIENT	1

bool _global_fDebug;

unsigned long hash(void* buf, int size) {
	unsigned long	h = 5361;
	for (int i = 0; i < size; i++) {
		h = ((h << 5) + h + ((char*)buf)[i]);
	}
	return h;
}

int main(int argc, char* argv[]) {
	int	option;
	int	mode = MODE_CLIENT;

	_global_fDebug = false;

	/* 
	 * socks :
	 * クライアントモードの時は使用するNWデバイスに対応したソケット情報を、
	 * サーバモードの時はそれぞれの接続元に対応したソケット情報を格納する
	 */
	std::vector<std::string>	device;

	//while ((option = getopt(argc, argv, "d:t:sc:")) > 0) {
	while ((option = getopt(argc, argv, "i:ds")) > 0) {
		switch (option) {
		case 'i':
			device.emplace_back(optarg);
			break;

		case 'd':
			_global_fDebug = true;
			break;

		case 't':
			break;
		
		case 's':
			mode = MODE_SERVER;
			break;

		case 'c':
			break;
		}
	}
	char	tun_name[64] = "tun_test";		// データを受け取る tun デバイス

	const char	*dst_addr	= "163.44.119.43";
	const int	dst_port	= 45555;

	// エラーチェック
	if (*tun_name == '\0') {
		print_error("tun device name not specified\n");
		exit(1);
	}
	if (mode == MODE_CLIENT) {
		// クライアントモード
		if (device.size() == 0) {
			print_error("any eth device not specified\n");
			exit(1);
		}
		auto client = unique_ptr<MPUDPTunnelClient>(new MPUDPTunnelClient(BUFSIZE));

		if (!client) { exit(1); }

		for (auto& d : device) { client->AddDevice(d); }
		
		if (!client->Connect(tun_name, dst_addr, dst_port)) { exit(1); };
		if (!client->MainLoop()) { exit(1); }
	}
	else {
		// サーバーモード
		auto server = unique_ptr<MPUDPTunnelServer>(new MPUDPTunnelServer(BUFSIZE));

		if (!server) { exit(1); }
		if (!server->Listen(tun_name, dst_port)) { exit(1); }
		if (!server->MainLoop()) { exit(1); }
	}
	return 0;
}