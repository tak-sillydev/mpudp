#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

// union のメモリ配置の仕様の関係で、
// partsに値を入れるとaddressから正しいエンディアンで取り出せる
typedef union {
	unsigned int	address;
	struct {
		unsigned char	a0, a1, a2, a3;
	} parts;
} IPV4_ADDR;

#define	PORT_TX	1111	// 送り側 srt-live-transmit は相手側コンピュータのこのポートに送る
#define	PORT_RX	2222	// 受け側 srt-live-transmit は自分側コンピュータのこのポートからデータを取る
#define	PATHWAY_NUM	2

int main(void) {
	fd_set	fds, rdfds;
	int		nfds;

	const char* devlist[PATHWAY_NUM] = {"enp0s25", "wlp2s0"};
	const int	npathway = PATHWAY_NUM;

	sockaddr_in	addr_tx_in, addr_tx_out;
	sockaddr_in	*addr_rx_in, addr_rx_out;
	IPV4_ADDR	v4addr;

	int			sock_tx, *sock_rx;	// それぞれTX側、RX側と通信するためのソケット
	int			rxmax = 0, counter = 0;

	timeval		tv;
	int			optval = 1;

	unsigned int	szaddr = sizeof(addr_tx_out);
	ssize_t			szrecv, selret;

	char	txbuf[2048], rxbuf[2048];

	// TX側ソケット準備
	sock_tx = socket(AF_INET, SOCK_DGRAM, 0);

	if (sock_tx == -1) { printf("FATAL: create TX socket\n"); return 0; }

	setsockopt(sock_tx, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

	// リレーTX側　待受ポート指定
	addr_tx_in.sin_family		= AF_INET;
	addr_tx_in.sin_addr.s_addr	= INADDR_ANY;
	addr_tx_in.sin_port			= htons(PORT_TX);
	bind(sock_tx, (sockaddr *)&addr_tx_in, sizeof(addr_tx_in));

	FD_ZERO(&rdfds);
	FD_SET(sock_tx, &rdfds);

	// RX側ソケット準備
	sock_rx = new int[npathway];
	addr_rx_in = new sockaddr_in[npathway];

	if (sock_rx == nullptr) { printf("FATAL: allocate rx socket\n"); return 0; }

	for (int i = 0; i < npathway; i++) {
		sock_rx[i] = socket(AF_INET, SOCK_DGRAM, 0);

		if (sock_rx[i] == -1) { printf("FATAL: create RX socket\n"); return 0; }

		setsockopt(sock_rx[i], SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
		setsockopt(sock_rx[i], SOL_SOCKET, SO_BINDTODEVICE, devlist[i], strlen(devlist[i]));

		// リレーRX側　待受ポート指定
		addr_rx_in[i].sin_family		= AF_INET;
		addr_rx_in[i].sin_addr.s_addr	= INADDR_ANY;
		addr_rx_in[i].sin_port			= 0;
		bind(sock_rx[i], (sockaddr *)&addr_rx_in[i], sizeof(sockaddr_in));
		getsockname(sock_rx[i], (sockaddr *)&addr_rx_in[i], &szaddr);		// bind() によって使用ポートが割り当てられたので書き戻し

		// リレーRX側　接続先ポート指定
		v4addr.parts.a0 = 192;
		v4addr.parts.a1 = 168;
		v4addr.parts.a2 =   0;
		v4addr.parts.a3 =  11;
		addr_rx_out.sin_family		= AF_INET;
		addr_rx_out.sin_addr.s_addr	= v4addr.address;
		addr_rx_out.sin_port		= htons(2222);

		FD_SET(sock_rx[i], &rdfds);
		rxmax = (rxmax > sock_rx[i]) ? rxmax : sock_rx[i];
	}
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
			v4addr.address = addr_tx_out.sin_addr.s_addr;
			printf(
				"FROM TX %d.%d.%d.%d:%d (%ldbytes) --> [RELAY IN %d > ",
				v4addr.parts.a0, v4addr.parts.a1, v4addr.parts.a2, v4addr.parts.a3,
				htons(addr_tx_out.sin_port), szrecv, htons(addr_tx_in.sin_port)
			);

			// 受信したデータをRX側へ流す
			sendto(sock_rx[counter], txbuf, szrecv, 0, (sockaddr *)&addr_rx_out, sizeof(addr_rx_out));

			v4addr.address = addr_rx_out.sin_addr.s_addr;
			printf(
				"OUT %d] --> TO RX %d.%d.%d.%d:%d\n",
				htons(addr_rx_in[counter].sin_port),
				v4addr.parts.a0, v4addr.parts.a1, v4addr.parts.a2, v4addr.parts.a3, htons(addr_rx_out.sin_port)
			);
			if (++counter == npathway) { counter = 0; }
		}
		for (int i = 0; i < npathway; i++) {
			if (FD_ISSET(sock_rx[i], &fds)) {
				// RX側からデータ受信
				// リレーRX側　接続先ポート取得（ユーザ指定値なのでやらなくてもいい）
				szrecv = recvfrom(sock_rx[i], rxbuf, 2048, MSG_WAITALL, (sockaddr *)&addr_rx_out, &szaddr);

				v4addr.address = addr_rx_out.sin_addr.s_addr;
				printf(
					"FROM RX %d.%d.%d.%d:%d (%ldbytes) --> [RELAY IN %d > ",
					v4addr.parts.a0, v4addr.parts.a1, v4addr.parts.a2, v4addr.parts.a3,
					htons(addr_rx_out.sin_port), szrecv, htons(addr_rx_in[i].sin_port)
				);

				// 受信したデータをTX側へ流す
				sendto(sock_tx, rxbuf, szrecv, 0, (sockaddr *)&addr_tx_out, sizeof(addr_tx_out));

				v4addr.address = addr_tx_out.sin_addr.s_addr;
				printf(
					"OUT %d] --> TO TX %d.%d.%d.%d:%d\n",
					htons(addr_tx_in.sin_port),
					v4addr.parts.a0, v4addr.parts.a1, v4addr.parts.a2, v4addr.parts.a3, htons(addr_tx_out.sin_port)
				);
			}
		}
	}
	close(sock_tx);
	for (int i = 0; i < npathway; i++) { close(sock_rx[i]); }
	return 0;
}