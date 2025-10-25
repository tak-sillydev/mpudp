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
#include "client.h"
#include "server.h"
#include "ringbuf.h"

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
	std::vector<SOCKET_PACK>	socks;
	std::vector<std::string>	device;

	socks.reserve(10);	// とりあえず10デバイス分のメモリを確保しておく

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
	if (mode == MODE_CLIENT && device.size() == 0) {
		print_error("any eth device not specified\n");
		exit(1);
	}

	int	sock_tun, sock_recv;

	// TUN デバイスの確保（デバイスは事前に要セットアップ）
	if ((sock_tun = tun_alloc(tun_name)) < 0) {
		print_error("Couldn't connect to tun device - %s\n", tun_name);
		exit(1);
	}
	pdebug("CONNECT OK - %s\n", tun_name);

	if (mode == MODE_CLIENT) {
		// クライアントモード
		// getaddrinfo() : hints に条件を入れてアドレスやらポートを指定すると
		// いい感じにほかのパラメータを補って res に結果を入れて返す
		addrinfo	*res;

		if (!client::getaddress(dst_addr, dst_port, &res)) { exit(1); }

		for (auto& d : device) {
			SOCKET_PACK	s;
			if (!client::setup(s, d, res)) { freeaddrinfo(res); exit(1); }
			socks.emplace_back(std::move(s));
		}
		freeaddrinfo(res);

		auto t = client::send_echo_thread(socks);
		client::main_loop(sock_tun, socks);

		t->join();
	}
	else {
		// サーバーモード
		if (!server::setup(sock_recv, dst_port)) { exit(1); }
		if (!server::main_loop(sock_tun, sock_recv, socks)) {
			close(sock_recv);
		}
	}
	close(sock_tun);
	socks.clear();

	return 0;
}