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

using namespace std;


#define	MODE_SERVER	0
#define	MODE_CLIENT	1
#define	BUFSIZE		2048

#define	max(a, b)	(((a) > (b)) ? (a) : (b))


unsigned long hash(void* buf, int size) {
	unsigned long	h = 5361;
	for (int i = 0; i < size; i++) {
		h = ((h << 5) + h + ((char*)buf)[i]);
	}
	return h;
}


int main(int argc, char* argv[]) {
	fd_set	rfds;
	int		max_fds, err;


	int	option;
	int	mode = MODE_CLIENT;

	/* 
	 * socks :
	 * クライアントモードの時は使用するNWデバイスに対応したソケット情報を、
	 * サーバモードの時はそれぞれの接続元に対応したソケット情報を格納する
	 */
	std::vector<SOCKET_PACK>	socks;
	socks.reserve(10);	// とりあえず10デバイス分のメモリを確保しておく

	//while ((option = getopt(argc, argv, "d:t:sc:")) > 0) {
	while ((option = getopt(argc, argv, "i:s")) > 0) {
		SOCKET_PACK	s;

		switch (option) {
		case 'i':
			s.eth_name = optarg;
			socks.emplace_back(std::move(s));
			break;

		case 'd':
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

	int	sock_tun;

	// TUN デバイスの確保（デバイスは事前に要セットアップ）
	if ((sock_tun = tun_alloc(tun_name)) < 0) {
		print_error("Couldn't connect to tun device - %s\n", tun_name);
		exit(1);
	}
	print_debug("CONNECT OK - %s\n", tun_name);

	int	eth_max = 0;
	int	optval = 1;

	unsigned int	szaddr = sizeof(sockaddr_in);
	ssize_t			selret;

	char	sock_buf[sizeof(TUN_HEADER) + BUFSIZE];

	TUN_HEADER	*pbuf_head = (TUN_HEADER*)sock_buf;
	char		*pbuf_data = sock_buf + sizeof(TUN_HEADER);

	if (mode == MODE_CLIENT) {
		// クライアントモード
		// getaddrinfo() : hints に条件を入れてアドレスやらポートを指定すると
		// いい感じにほかのパラメータを補って res に結果を入れて返す
		char		port_str[16];
		addrinfo	hints, *res;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family		= AF_INET;
		hints.ai_socktype	= SOCK_DGRAM;

		sprintf(port_str, "%hu", dst_port);
		if ((err = getaddrinfo(dst_addr, port_str, &hints, &res)) < 0) {
			perror("getaddrinfo()");
			print_error("err = %d, reason = %s\n", err, gai_strerror(err));
			print_error("errno = %d\n", errno);
			exit(1);
		}

		for (auto& s : socks) {
			// 使用する実デバイスにそれぞれソケットをくっつける
			// ソケットの作成とオプションの設定
			if ((s.sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0) {
				perror("socket()");
				print_error("Couldn't create socket - %s\n", s.eth_name.c_str());
				print_error("errno = %d\n", errno);
				exit(1);
			}
			if (setsockopt(s.sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
				perror("setsockopt()");
				print_error("Couldn't set value of SO_REUSEASSR - %s\n", s.eth_name.c_str());
				print_error("errno = %d\n", errno);
				exit(1);
			}
			if (setsockopt(s.sock_fd, SOL_SOCKET, SO_BINDTODEVICE, s.eth_name.c_str(), s.eth_name.length()) < 0) {
				perror("setsockopt()");
				print_error("Couldn't set value of SO_BINDTODEVICE - %s\n", s.eth_name.c_str());
				print_error("errno = %d\n", errno);
				exit(1);
			}

			// 接続先の設定
			if (connect(s.sock_fd, res->ai_addr, res->ai_addrlen) < 0) {
				perror("connect()");
				print_error("device = %s, errno = %d\n", s.eth_name.c_str(), errno);
				exit(1);
			}
			s.remote_addr = *(sockaddr_in *)res->ai_addr;

			// ローカル側で使用するポートの割当
			s.local_addr.sin_family			= AF_INET;
			s.local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
			s.local_addr.sin_port			= htons(0);
			bind(s.sock_fd, (sockaddr*)&(s.local_addr), sizeof(sockaddr));
			getsockname(s.sock_fd, (sockaddr*)&(s.local_addr), &szaddr);	// bind() によって使用ポートが割り当てられたので情報を取得

			print_debug("eth[%s]: fd: %d, local addr: %s, port: %d\n",
				s.eth_name.c_str(),
				s.sock_fd,
				inet_ntoa(s.local_addr.sin_addr),
				ntohs(s.local_addr.sin_port)
			);
			eth_max = max(s.sock_fd, eth_max);
		}
		freeaddrinfo(res);
	}
	else {
		// サーバーモード
		// サーバーモードでは、リストの先頭が接続待ち受けソケットになる
		socks.clear();
		SOCKET_PACK	s;

		// ソケットの作成とオプションの設定
		if ((s.sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			perror("socket()");
			print_error("Couldn't create socket\n");
			print_error("errno = %d\n", errno);
			exit(1);
		}
		if (setsockopt(s.sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_REUSEASSR\n");
			print_error("errno = %d\n", errno);
		}
		memset(&s.local_addr, 0, sizeof(s.local_addr));
		s.local_addr.sin_family			= AF_INET;
		s.local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
		s.local_addr.sin_port			= htons(dst_port);

		if (bind(s.sock_fd, (sockaddr*)&s.local_addr, sizeof(s.local_addr)) < 0) {
			perror("bind()");
			print_error("errno = %d\n", errno);
			exit(1);
		}
		getsockname(s.sock_fd, (sockaddr*)&s.local_addr, &szaddr);
		print_debug("eth: addr: %s, port: %d\n",
			inet_ntoa(s.local_addr.sin_addr),
			ntohs(s.local_addr.sin_port)
		);
		eth_max = s.sock_fd;
		socks.emplace_back(std::move(s));
	}
	memset(sock_buf, 0, sizeof(TUN_HEADER) + BUFSIZE);
	max_fds = max(sock_tun, eth_max);

	int		nread, nwrite, tun_cnt = 0, eth_cnt = 0;
	auto 	socks_it = socks.begin();

	while (true) {

		// サーバーモードの時、sock_fd は全部同じ値なので気にしなくて良い
		FD_ZERO(&rfds);
		FD_SET(sock_tun, &rfds);
		print_debug("sock_tun = %d, eth_max = %d, max_fds = %d\n", sock_tun, eth_max, max_fds);
		for (auto& s : socks) { print_debug("sock_fd = %d\n", s.sock_fd); FD_SET(s.sock_fd, &rfds); }
		
		selret = select(max_fds + 1, &rfds, NULL, NULL, NULL);	// データ到着まで待機

		if (selret < 0) {
			if (errno == EINTR) continue;

			perror("select()");
			print_error("errno = %d\n", errno);
			goto end;
		}
		if (FD_ISSET(sock_tun, &rfds)) {
			// TUN にデータが入った
			// TUN から取れるデータはIPパケットそのもの（IPヘッダが存在する）
			try {
				nread = tun_eread(sock_tun, (void*)pbuf_data, BUFSIZE);
				print_debug("from tun seq=%d : read %lu bytes\n", tun_cnt, nread);
				print_debug("IP header says: packet length = %lu\n", ntohs(((unsigned short*)pbuf_data)[1]));
				print_debug("\tpbuf_head: %02X %02X %02X %02X %02X %02X %02X %02X\n",
					((unsigned char*)pbuf_head)[0], ((unsigned char*)pbuf_head)[1], ((unsigned char*)pbuf_head)[2], ((unsigned char*)pbuf_head)[3],
					((unsigned char*)pbuf_head)[4], ((unsigned char*)pbuf_head)[5], ((unsigned char*)pbuf_head)[6], ((unsigned char*)pbuf_head)[7]
				);
				print_debug("pbuf_data: %02X %02X %02X %02X %02X %02X %02X %02X\n",
					(unsigned char)pbuf_data[0], (unsigned char)pbuf_data[1], (unsigned char)pbuf_data[2], (unsigned char)pbuf_data[3],
					(unsigned char)pbuf_data[4], (unsigned char)pbuf_data[5], (unsigned char)pbuf_data[6], (unsigned char)pbuf_data[7]
				);

				pbuf_head->mode = (char)MODE_SPEED;
				pbuf_head->reserved = 0;
				pbuf_head->length = (unsigned short)nread;	// データ ペイロード長
				pbuf_head->stable_id = 0;

				nread += sizeof(TUN_HEADER);

				if (mode == MODE_CLIENT) {
					// クライアントモード
					// 現状、ラウンドロビン送信
					nwrite = sendto(
						socks_it->sock_fd, sock_buf, nread, 0,
						(sockaddr*)&socks_it->remote_addr, sizeof(socks_it->remote_addr)
					);

					print_debug("device = %s\n", socks_it->eth_name.c_str());
					socks_it++;
					if (socks_it == socks.cend()) { socks_it = socks.begin(); }
				}
				else {
					/* 
					 * サーバーモード、socks の最初の要素以外にはデータの送信元を表すアドレスのみ格納
					 * sock_fd には先頭の要素と同じものを格納する（bind は行わない）
					 * 最初の要素は待ち受けに使われているソケット
					 * MODE_STABLE より、サーバからはすべての経路に対して同じパケットを送って冗長化
					 */
					print_debug("server to eth, socks len = %d\n", socks.size());
					pbuf_head->mode = (char)MODE_STABLE;

					for (size_t i = 1; i < socks.size(); i++) {
						nwrite = sendto(
							socks[i].sock_fd, sock_buf, nread, 0,
							(sockaddr*)&(socks[i].remote_addr), sizeof(socks[i].remote_addr)
						);
						print_debug(
							"to eth addr: %s, port: %d\n",
							inet_ntoa(socks[i].remote_addr.sin_addr),
							ntohs(socks[i].remote_addr.sin_port)
						);
					}
				}
				print_debug("to eth seq=%d : write %lu bytes\n", tun_cnt, nwrite);
			}
			catch (std::exception &e) {
				print_error("eread() / ewrite(): %s - the data will be discarded. Continue.\n", e.what());
			}
			tun_cnt++;
		}
		if (mode == MODE_CLIENT) {
			/*
			 * クライアントモード
			 * クライアントモードモードでは、socks の各要素はそれぞれの eth デバイスに割り当てられたソケット
			 * イテレータを走査してデータを受信する
			 */
			for (auto& s : socks) {
				if (FD_ISSET(s.sock_fd, &rfds)) {
					// eth[i] にデータが入った
					/* 
					 * パケットサイズがMTUを超える場合、パケットは複数に分割される
					 * この分割、また受信時の再合成の処理はより低いレイヤー（ネットワーク層）で行われるので、
					 * UDPのレイヤでは特に考えなくて良い。ただし、パケットが遅れて到着する可能性はあるので、
					 * 送信したバイト数を読み切るまで待機する処理が必要（ここでは readn が行う）
					 */
					sockaddr_in	addr_from;
					socklen_t	from_len;
					int	n;

					try {
						from_len = sizeof(sockaddr_in);
						print_debug("device = %s, fd = %d\n", s.eth_name.c_str(), s.sock_fd);
						n = recvfrom(
							s.sock_fd, sock_buf, sizeof(TUN_HEADER),
							MSG_PEEK, (sockaddr*)&addr_from, &from_len
						);
						if (n < 0) { throw std::runtime_error("recvfrom returned an invalid value"); }
						nread = n;

						print_debug("from eth seq=%d : read %lu bytes\n", eth_cnt, nread);
						// パケット分割に備え、対向側が TUN_HEADER に書き込んだデータ長を読み切るまで待機
						n = recvfrom(
							s.sock_fd, sock_buf, sizeof(TUN_HEADER) + pbuf_head->length,
							MSG_WAITALL, (sockaddr*)&addr_from, &from_len
						);
						if (n < 0) { throw std::runtime_error("recvfrom returned an invalid value"); }
						nread += n;
						
						print_debug("from eth seq=%d : read %lu bytes\n", eth_cnt, nread);
						print_debug("IP header says: packet length = %lu\n", ntohs(((unsigned short*)pbuf_data)[1]));
						print_debug("addr: %s, port: %d\n",	inet_ntoa(addr_from.sin_addr), ntohs(addr_from.sin_port));

						nwrite = tun_ewrite(sock_tun, pbuf_data, pbuf_head->length);
						print_debug("to tun seq=%d : write %lu bytes\n", eth_cnt, nwrite);
					}
					catch (std::exception &e) {
						perror("recvfrom");
						print_debug("errno = %d\n", errno);
						print_error("recvfrom: %s - the data will be discarded. Continue.\n", e.what());
						goto end;
					}
					eth_cnt++;
				}
			}
		}
		else {
			/*
			 * サーバーモード
			 * サーバーモードでは、監視対象はリストの先頭のソケットのみ
			 * あとのソケットは使用されず、経路情報だけを参照する
			 */
			const auto&	sock_recv = socks[0];

			if (FD_ISSET(sock_recv.sock_fd, &rfds)) {
				sockaddr_in	addr_from;
				socklen_t	from_len;
				int	n;

				try {
					from_len = sizeof(sockaddr_in);
					n = recvfrom(
						sock_recv.sock_fd, sock_buf, sizeof(TUN_HEADER),
						MSG_PEEK, (sockaddr*)&addr_from, &from_len
					);
					if (n < 0) { throw std::runtime_error("recvfrom returned an invalid number"); }
					nread = n;
					print_debug("\tfrom eth seq=%d : read %lu bytes\n", eth_cnt, nread);

					// パケット分割に備え、対向側が TUN_HEADER に書き込んだデータ長を読み切るまで待機
					n = recvfrom(
						sock_recv.sock_fd, sock_buf, sizeof(TUN_HEADER) + pbuf_head->length,
						MSG_WAITALL, (sockaddr*)&addr_from, &from_len
					);
					if (n < 0) { throw std::runtime_error("recvfrom returned an invalid number"); }
					nread += n;

					print_debug("---- SOCKET RECEIVED ----\n");
					print_debug("\tfrom eth seq=%d : read %lu bytes\n", eth_cnt, nread);
					print_debug("\ttun_head.length = %lu\n", pbuf_head->length);
					print_debug("\tIP header says: packet length = %lu\n", ntohs(((unsigned short*)pbuf_data)[1]));
					print_debug("\tpbuf_head: %02X %02X %02X %02X %02X %02X %02X %02X\n",
						((unsigned char*)pbuf_head)[0], ((unsigned char*)pbuf_head)[1], ((unsigned char*)pbuf_head)[2], ((unsigned char*)pbuf_head)[3],
						((unsigned char*)pbuf_head)[4], ((unsigned char*)pbuf_head)[5], ((unsigned char*)pbuf_head)[6], ((unsigned char*)pbuf_head)[7]
					);
					print_debug("\tpbuf_data: %02X %02X %02X %02X %02X %02X %02X %02X\n",
						(unsigned char)pbuf_data[0], (unsigned char)pbuf_data[1], (unsigned char)pbuf_data[2], (unsigned char)pbuf_data[3],
						(unsigned char)pbuf_data[4], (unsigned char)pbuf_data[5], (unsigned char)pbuf_data[6], (unsigned char)pbuf_data[7]
					);
					print_debug("\taddr: %s, port: %d\n", inet_ntoa(addr_from.sin_addr), ntohs(addr_from.sin_port));

					nwrite = tun_ewrite(sock_tun, pbuf_data, pbuf_head->length);
					print_debug("to tun seq=%d : write %lu bytes\n", eth_cnt, nwrite);
				}
				catch (std::exception &e) {
					perror("recvfrom");
					print_debug("errno = %d\n", errno);
					print_error("recvfrom: %s - the data will be discarded. Continue.\n", e.what());
				}
				eth_cnt++;

				// 今までにない経路からの通信なら返信リストに登録
				for (size_t i = 0; i < socks.size(); i++) {
					if (is_same_addr(addr_from, socks[i].remote_addr) && i != 0) { break; }
					if (i >= socks.size() - 1) {
						SOCKET_PACK	s_tmp;

						s_tmp.sock_fd		= socks.begin()->sock_fd;
						s_tmp.local_addr 	= socks.begin()->local_addr;
						s_tmp.remote_addr	= addr_from;
						socks.emplace_back(std::move(s_tmp));
					}
				}
			}
		}
	}
end:
	close(sock_tun);
	socks.clear();

	return 0;
}