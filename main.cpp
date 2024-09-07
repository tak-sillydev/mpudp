/*
	試作品：IPv4 only 2024-09-07
*/
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fstream>
#include <iostream>

#include "json.hpp"

using namespace std;

int main(void) {
	fd_set	fds, rdfds;
	int		nfds;

	ifstream	ifs("./config.json");

	if (ifs.fail()) {
		cout << "FAIL: open config.json" << endl;
		exit(1);
	}
	auto	config = nlohmann::json::parse(ifs);
	auto	device_list = config["device_list"];
	int		ndevices = device_list.size();

	// tx_out (CAM側,接続元) <--> [ tx_in - rx_in ] <--> rx_out（モニタ側,接続先）
	sockaddr_in6	addr_tx_in, addr_tx_out;
	sockaddr_in		*addr_rx_in, addr_rx_out;

	int			sock_tx, *sock_rx;	// それぞれTX側、RX側と通信するためのソケット
	int			rxmax = 0, counter = 0;

	timeval		tv;
	int			optval = 1;

	unsigned int	szaddr = sizeof(addr_tx_out);
	ssize_t			szrecv, selret;

	char	txbuf[2048], rxbuf[2048];
	char	addrbuf[64];

	// TX側ソケット準備
	sock_tx = socket(AF_INET6, SOCK_DGRAM, 0);

	if (sock_tx == -1) { printf("FATAL: create TX socket\n"); return 0; }

	setsockopt(sock_tx, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	// リレーTX側　待受ポート指定
	addr_tx_in.sin6_family		= AF_INET6;
	addr_tx_in.sin6_addr		= IN6ADDR_ANY_INIT;
	addr_tx_in.sin6_port		= htons(config["receive_at"]["port"]);
	bind(sock_tx, (sockaddr *)&addr_tx_in, sizeof(addr_tx_in));

	FD_ZERO(&rdfds);
	FD_SET(sock_tx, &rdfds);

	// RX側ソケット準備
	sock_rx = new int[ndevices];
	addr_rx_in = new sockaddr_in[ndevices];

	if (sock_rx == nullptr) {
		cout << "FATAL: allocate RX socket" << endl;
		exit(1);
	}
	string		sendto_addr = config["send_to"]["address"];
	uint16_t	sendto_port = config["send_to"]["port"];
	char		port_str[16];
	addrinfo	hints, *res;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family		= AF_INET;
	hints.ai_socktype	= SOCK_DGRAM;

	sprintf(port_str, "%hu", sendto_port);
	if (getaddrinfo(sendto_addr.c_str(), port_str, &hints, &res) < 0) {
		cout << "FATAL: get address info" << endl;
		exit(1);
	}
	// リレーRX側　接続先ポート指定
	addr_rx_out = *reinterpret_cast<sockaddr_in*>(res->ai_addr);

	for (int i = 0; i < ndevices; i++) {
		const char	*device = device_list[i].get<string>().c_str();

		sock_rx[i] = socket(res->ai_family, res->ai_socktype, 0);

		if (sock_rx[i] == -1) { cout << "FATAL: create RX socket" << endl; return 0; }

		setsockopt(sock_rx[i], SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
		setsockopt(sock_rx[i], SOL_SOCKET, SO_BINDTODEVICE, device, strlen(device));

		// リレーRX側　待受ポート指定
		addr_rx_in[i].sin_family		= AF_INET;
		addr_rx_in[i].sin_addr.s_addr	= INADDR_ANY;
		addr_rx_in[i].sin_port			= 0;
		bind(sock_rx[i], (sockaddr *)&addr_rx_in[i], sizeof(sockaddr_in));
		getsockname(sock_rx[i], (sockaddr *)&addr_rx_in[i], &szaddr);		// bind() によって使用ポートが割り当てられたので書き戻し

		FD_SET(sock_rx[i], &rdfds);
		rxmax = (rxmax > sock_rx[i]) ? rxmax : sock_rx[i];
	}
	freeaddrinfo(res);
	nfds = (sock_tx > rxmax) ? sock_tx : rxmax;

	memset(txbuf, 0, 2048);
	memset(rxbuf, 0, 2048);

	while (true) {
		tv.tv_sec	= 0;
		tv.tv_usec	= 500 * 1000;

		memcpy(&fds, &rdfds, sizeof(fds));
		selret = select(nfds + 1, &fds, NULL, NULL, &tv);	// データ到着まで待機、タイムアウト500ミリ秒

		if (selret < 0) {
			// エラー発生時
			printf("error occured in select\n");	break;
		}
		else if (selret == 0) {
			// タイムアウト発生時
			continue;
		}

		if (FD_ISSET(sock_tx, &fds)) {
			// TX側からデータ受信
			// リレーTX側　接続先ポート取得
			szrecv = recvfrom(sock_tx, txbuf, 2048, MSG_WAITALL, (sockaddr *)&addr_tx_out, &szaddr);

			if (szrecv == -1) {
				//printerr()
			}
			inet_ntop(addr_tx_out.sin6_family, &addr_tx_out.sin6_addr, addrbuf, sizeof(addrbuf));
			cout << "FROM TX " << addrbuf << ":" << htons(addr_tx_out.sin6_port) <<
				"(" << szrecv << "bytes) " <<
				"--> [RELAY IN " << htons(addr_tx_in.sin6_port) << " > ";

			// 受信したデータをRX側へ流す
			sendto(sock_rx[counter], txbuf, szrecv, 0, (sockaddr *)&addr_rx_out, sizeof(addr_rx_out));

			inet_ntop(addr_rx_out.sin_family, &addr_rx_out.sin_addr, addrbuf, sizeof(addrbuf));
			cout << "OUT " << htons(addr_rx_in[counter].sin_port) << "] --> TO RX " <<
				addrbuf << ":" << htons(addr_rx_out.sin_port) << endl;
			
			if (++counter == ndevices) { counter = 0; }
		}
		for (int i = 0; i < ndevices; i++) {
			if (FD_ISSET(sock_rx[i], &fds)) {
				// RX側からデータ受信
				// リレーRX側　接続先ポート取得（ユーザ指定値なのでやらなくてもいい）
				szrecv = recvfrom(sock_rx[i], rxbuf, 2048, MSG_WAITALL, (sockaddr *)&addr_rx_out, &szaddr);

				inet_ntop(addr_rx_out.sin_family, &addr_rx_out.sin_addr, addrbuf, sizeof(addrbuf));
				cout << "FROM RX " << addrbuf << ":" << htons(addr_rx_out.sin_port) <<
					"(" << szrecv << "bytes) " <<
					"--> [RELAY IN " << htons(addr_rx_in[i].sin_port) << "> ";

				// 受信したデータをTX側へ流す
				sendto(sock_tx, rxbuf, szrecv, 0, (sockaddr *)&addr_tx_out, sizeof(addr_tx_out));

				inet_ntop(addr_tx_out.sin6_family, &addr_tx_out.sin6_addr, addrbuf, sizeof(addrbuf));
				cout << "OUT " << htons(addr_tx_in.sin6_port) << "] --> TO TX " <<
					addrbuf << ":" << htons(addr_tx_out.sin6_port) << endl;
			}
		}
	}
	close(sock_tx);
	for (int i = 0; i < ndevices; i++) { close(sock_rx[i]); }

	return 0;
}