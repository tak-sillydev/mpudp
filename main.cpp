/*
	試作品：IPv4 only 2024-09-07
*/
#include <unistd.h>
#include <errno.h>

#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>

#include "print.h"
#include "network.h"
#include "client.h"
#include "server.h"
#include "ringbuf.h"

using namespace std;


#define	MODE_SERVER	0
#define	MODE_CLIENT	1
#define	BUFSIZE		2048

bool _global_fDebug;

unsigned long hash(void* buf, int size) {
	unsigned long	h = 5361;
	for (int i = 0; i < size; i++) {
		h = ((h << 5) + h + ((char*)buf)[i]);
	}
	return h;
}

int main(int argc, char* argv[]) {
	fd_set	rfds;
	int		max_fds;

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

	int	sock_tun, sock_recv = -1;

	// TUN デバイスの確保（デバイスは事前に要セットアップ）
	if ((sock_tun = tun_alloc(tun_name)) < 0) {
		print_error("Couldn't connect to tun device - %s\n", tun_name);
		exit(1);
	}
	pdebug("CONNECT OK - %s\n", tun_name);

	char	sock_buf[sizeof(TUN_HEADER) + BUFSIZE];

	TUN_HEADER	*pbuf_head = (TUN_HEADER*)sock_buf;
	char		*pbuf_data = sock_buf + sizeof(TUN_HEADER);

	if (mode == MODE_CLIENT) {
		// クライアントモード
		// getaddrinfo() : hints に条件を入れてアドレスやらポートを指定すると
		// いい感じにほかのパラメータを補って res に結果を入れて返す
		addrinfo	*res;

		if (!client::getaddress(dst_addr, dst_port, &res)) { exit(1); }

		for (auto& d : device) {
			SOCKET_PACK	s;
			if (!client::setup(s, d, res, max_fds)) { freeaddrinfo(res); exit(1); }
			socks.emplace_back(std::move(s));
		}
		freeaddrinfo(res);
	}
	else {
		// サーバーモード
		// サーバーモードでは、リストの先頭が接続待ち受けソケットになる
		if (!server::setup(sock_recv, dst_port)) { exit(1); }
		
		max_fds = sock_recv;
	}
	memset(sock_buf, 0, sizeof(TUN_HEADER) + BUFSIZE);
	max_fds = max(sock_tun, max_fds);

	int		nread, nwrite;
	int		tun_seq = 0, eth_seq = 0, stable_id = 0;
	auto 	socks_it = socks.begin();

	ringbuf<decltype(pbuf_head->seq), 32>	seq_rec(-1);

	while (true) {
		// 初期化・使用するソケットの通知
		FD_ZERO(&rfds);
		FD_SET(sock_tun, &rfds);

		if (mode == MODE_CLIENT) {
			for (auto& s : socks) { FD_SET(s.sock_fd, &rfds); }
		}
		else {
			FD_SET(sock_recv, &rfds);
		}

		// データ到着まで待機
		if (select(max_fds + 1, &rfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR) continue;

			perror("select()");
			print_error("errno = %d\n", errno);
			goto end;
		}

		// データが到着、データを受信したソケットの判別とデータの取り出し
		if (mode == MODE_CLIENT) {
			/*
			 * クライアントモード
			 * クライアントモードモードでは、socks の各要素はそれぞれの eth デバイスに割り当てられたソケット
			 * イテレータを走査してデータを受信する
			 */
			if (FD_ISSET(sock_tun, &rfds)) {
				/* 
				 * TUN デバイス側からデータを受信
				 * ここに書き込まれるデータは生のIPパケット
				 * ETH デバイスを選定してデータを書き込む（ネットワーク側に流す）
				 */
				try {
					pdebug("\n===== TUN DEVICE RECEIVED DATA =====\n");
					nread = tun_eread(sock_tun, (void*)pbuf_data, BUFSIZE);
					pdebug_tunrecv(tun_seq, nread, pbuf_data);
					tun_seq++;

					pbuf_head->mode = (char)MODE_SPEED;
					pbuf_head->device_id = (unsigned char)(socks_it->sock_fd & 0xff);	// 雑だなぁ
					pbuf_head->length = (unsigned short)nread;	// データ ペイロード長
					pbuf_head->seq = 0;

					// ラウンドロビンでデータを送る
					// ここにパケットを効率よく分散する機構を組み込む
					nwrite = sendto(
						socks_it->sock_fd, sock_buf, nread + sizeof(TUN_HEADER), 0,
						(sockaddr*)&socks_it->remote_addr, sizeof(socks_it->remote_addr)
					);
					if (nwrite < 0) { throw std::runtime_error("sendto returned an invalid value"); }

					pdebug("packet was sent to eth device = %s: %lu bytes\n", socks_it->eth_name.c_str(), nwrite);

					socks_it++;
					if (socks_it == socks.cend()) { socks_it = socks.begin(); }
				} catch (std::exception& e) {
					perror("eread / sendto");
					print_error("errno = %d\n", errno);
					print_error("%s - the data will be discarded. Continue.\n", e.what());
				}
			}
			for (auto& s : socks) {
				if (FD_ISSET(s.sock_fd, &rfds)) {
					/* 
					 * ETH デバイス側からデータを受信
					 * パケットサイズがMTUを超える場合、パケットは複数に分割される
					 * この分割、また受信時の再合成の処理はより低いレイヤー（ネットワーク層）で行われるので、
					 * UDPのレイヤでは特に考えなくて良い。ただし、パケットが遅れて到着する可能性はあるので、
					 * 送信したバイト数を読み切るまで待機する処理が必要（ここでは readn が行う）
					 */
					sockaddr_in	addr_from;
					socklen_t	from_len;
					int	n;

					try {
						pdebug("\n===== ETH DEVICE [%s] RECEIVED DATA =====\n", s.eth_name.c_str());

						// 最初にヘッダの部分だけをピーク（チラ見）する
						from_len = sizeof(sockaddr_in);
						n = recvfrom(
							s.sock_fd, sock_buf, sizeof(TUN_HEADER),
							MSG_PEEK, (sockaddr*)&addr_from, &from_len
						);
						if (n < 0) { throw std::runtime_error("recvfrom returned an invalid value"); }
						nread = n;

						// パケット分割に備え、対向側が TUN_HEADER に書き込んだデータ長を読み切るまで待機
						n = recvfrom(
							s.sock_fd, sock_buf, sizeof(TUN_HEADER) + pbuf_head->length,
							MSG_WAITALL, (sockaddr*)&addr_from, &from_len
						);
						if (n < 0) { throw std::runtime_error("recvfrom returned an invalid value"); }
						nread += n;
						
						pdebug_ethrecv(eth_seq, nread, sock_buf, addr_from);

						if (pbuf_head->mode == MODE_STABLE) {
							const auto it = std::find(seq_rec.begin(), seq_rec.end(), pbuf_head->seq);

							if (it != seq_rec.end()) {
								pdebug("packet was already received: skip.\n");
								eth_seq++;
								continue;
							}
							seq_rec.push(pbuf_head->seq);
						}
						nwrite = tun_ewrite(sock_tun, pbuf_data, pbuf_head->length);
						pdebug("packet was sent to tun seq=%d : write %lu bytes\n", eth_seq, nwrite);
						eth_seq++;
					}
					catch (std::exception &e) {
						perror("recvfrom / ewrite");
						pdebug("errno = %d\n", errno);
						print_error("%s - the data will be discarded. Continue.\n", e.what());
						//goto end;
					}
				}
			}
		}
		else {
			/*
			 * サーバーモード
			 * サーバーモードでは、ソケットリストは経路情報だけを格納するものとして用い、
			 * データの送受信には用いない（代わりに待ち受けソケットを用いる）
			 * 転送モードはSTABLE、受信側で stable_id を確認して重複したものは破棄する
			 */
			if (FD_ISSET(sock_tun, &rfds)) {
				try {
					pdebug("\n===== TUN DEVICE RECEIVED DATA =====\n");
					nread = tun_eread(sock_tun, (void*)pbuf_data, BUFSIZE);
					pdebug_tunrecv(tun_seq, nread, pbuf_data);
					tun_seq++;

					pbuf_head->mode = (char)MODE_STABLE;
					pbuf_head->device_id = 0xff;
					pbuf_head->length = nread;
					pbuf_head->seq = stable_id++;
					nread += sizeof(TUN_HEADER);

					// それぞれのソケットリストに書かれたアドレスへパケットを送信
					for (auto& s : socks) {
						nwrite = sendto(
							sock_recv, sock_buf, nread, 0,
							(sockaddr*)&(s.remote_addr), sizeof(s.remote_addr)
						);
						if (nwrite < 0) { throw std::runtime_error("sendto returned an invalid value"); }
						pdebug(
							"packet was sent to : %s:%d\n",
							inet_ntoa(s.remote_addr.sin_addr), ntohs(s.remote_addr.sin_port)
						);
					}
				} catch (std::exception& e) {
					perror("eread / sendto");
					print_error("errno = %d\n");
					print_error("%s: %s - the data will be discarded. Continue.\n", e.what());
				}
			}
			if (FD_ISSET(sock_recv, &rfds)) {
				/*
				 * 待受中のソケットにデータが入った
				 */
				sockaddr_in	addr_from;
				socklen_t	from_len;
				int	n;

				try {
					pdebug("\n===== ETH DEVICE RECEIVED DATA =====\n");

					from_len = sizeof(sockaddr_in);
					n = recvfrom(
						sock_recv, sock_buf, sizeof(TUN_HEADER),
						MSG_PEEK, (sockaddr*)&addr_from, &from_len
					);
					if (n < 0) { throw std::runtime_error("recvfrom returned an invalid number"); }
					nread = n;

					// パケット分割に備え、対向側が TUN_HEADER に書き込んだデータ長を読み切るまで待機
					n = recvfrom(
						sock_recv, sock_buf, sizeof(TUN_HEADER) + pbuf_head->length,
						MSG_WAITALL, (sockaddr*)&addr_from, &from_len
					);
					if (n < 0) { throw std::runtime_error("recvfrom returned an invalid number"); }
					nread += n;

					pdebug_ethrecv(eth_seq, nread, sock_buf, addr_from);

					nwrite = tun_ewrite(sock_tun, pbuf_data, pbuf_head->length);
					pdebug("packet was sent to tun seq=%d : write %lu bytes\n", eth_seq, nwrite);
				}
				catch (std::exception &e) {
					perror("recvfrom / ewrite");
					pdebug("errno = %d\n", errno);
					print_error("%s - the data will be discarded. Continue.\n", e.what());
				}
				eth_seq++;

				// 今までにない経路からの通信なら返信リストに登録
				// デバイスIDが同じでも、ポート番号などアドレス情報が変わっていれば更新
				auto it = std::find_if(socks.begin(), socks.end(),
					[pbuf_head](const SOCKET_PACK& s){ return pbuf_head->device_id == s.sock_fd; }
				);
				if (it == socks.end()) {
					SOCKET_PACK	s_tmp;

					s_tmp.sock_fd		= pbuf_head->device_id;
					s_tmp.remote_addr	= addr_from;
					socks.emplace_back(std::move(s_tmp));
				}
				else if (!is_same_addr(it->remote_addr, addr_from)) {
					it->remote_addr = addr_from;
				}
			}
		}
	}
end:
	close(sock_tun);
	if (sock_recv != -1) { close(sock_recv); }

	socks.clear();

	return 0;
}