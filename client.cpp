#include <algorithm>
#include <stdexcept>

#include "print.h"
#include "client.h"
#include "ringbuf.h"

namespace client {
	bool getaddress(const char* dst_addr, const int dst_port, addrinfo **result) {
		char		port_str[16];
		addrinfo	hints;
		int			err;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family		= AF_INET;
		hints.ai_socktype	= SOCK_DGRAM;

		sprintf(port_str, "%hu", dst_port);
		if ((err = getaddrinfo(dst_addr, port_str, &hints, result)) < 0) {
			perror("getaddrinfo()");
			print_error("err = %d, reason = %s\n", err, gai_strerror(err));
			print_error("errno = %d\n", errno);
			return false;
		}
		return true;
	}

	bool setup(SOCKET_PACK& s, std::string& device_name, const addrinfo* pai) {
		// 使用する実デバイスにソケットを割り当てる
		// ソケットの作成とオプションの設定
		socklen_t	szaddr = sizeof(sockaddr_in);
		int			optval = 1;

		if ((s.sock_fd = socket(pai->ai_family, pai->ai_socktype, pai->ai_protocol)) < 0) {
			perror("socket()");
			print_error("Couldn't create socket - %s\n", device_name.c_str());
			print_error("errno = %d\n", errno);
			return false;
		}
		if (setsockopt(s.sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_REUSEASSR - %s\n", device_name.c_str());
			print_error("errno = %d\n", errno);
			return false;
		}
		if (setsockopt(s.sock_fd, SOL_SOCKET, SO_BINDTODEVICE, device_name.c_str(), device_name.length()) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_BINDTODEVICE - %s\n", device_name.c_str());
			print_error("errno = %d\n", errno);
			return false;
		}

		// 接続先の設定
		if (connect(s.sock_fd, pai->ai_addr, pai->ai_addrlen) < 0) {
			perror("connect()");
			print_error("device = %s, errno = %d\n", s.eth_name.c_str(), errno);
			return false;
		}
		s.remote_addr = *(sockaddr_in *)pai->ai_addr;

		// ローカル側で使用するポートの割当
		s.local_addr.sin_family			= AF_INET;
		s.local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
		s.local_addr.sin_port			= htons(0);
		bind(s.sock_fd, (sockaddr*)&(s.local_addr), sizeof(sockaddr));
		getsockname(s.sock_fd, (sockaddr*)&(s.local_addr), &szaddr);	// bind() によって使用ポートが割り当てられたので情報を取得

		s.eth_name = device_name;

		pdebug("eth[%s]: fd: %d, local addr: %s, port: %d\n",
			s.eth_name.c_str(),
			s.sock_fd,
			inet_ntoa(s.local_addr.sin_addr),
			ntohs(s.local_addr.sin_port)
		);
		return true;
	}

	/*
	 * クライアントモード
	 * クライアントモードモードでは、socks の各要素はそれぞれの eth デバイスに割り当てられたソケット
	 * イテレータを走査してデータを受信する
	 */
	bool main_loop(int sock_tun, std::vector<SOCKET_PACK>& socks) {
		fd_set	rfds;
		int		max_fd = -1;

		int	nread, nwrite;
		int	tun_seq = 0, eth_seq = 0;
		
		// データの送受信バッファ
		uint8_t		sock_buf[sizeof(TUN_HEADER) + BUFSIZE];
		TUN_HEADER	*pbuf_head = (TUN_HEADER*)sock_buf;
		uint8_t		*pbuf_data = sock_buf + sizeof(TUN_HEADER);

		auto socks_it = socks.begin();

		/*
		 * 受信済みのパケット番号を記録する場所
		 * MODE_STABLE で送信されたパケットは全部の経路に同じものを流して冗長化するので、
		 * 受信側で「すでに受信した」パケットは廃棄する必要がある。
		 * このバッファは溢れた場合、古いものから自動的に削除される仕組み。
		 */
		ringbuf<decltype(pbuf_head->seq), 32>	seq_rec(-1);


		std::for_each(
			socks.begin(), socks.end(),
			[&max_fd](const SOCKET_PACK& x){ max_fd = max(x.sock_fd, max_fd); }
		);
		max_fd = max(sock_tun, max_fd);
		pdebug("max_fd = %d\n", max_fd);
		pdebug("sock_tun = %d\n", sock_tun);
		pdebug("socks = ");
		for (const auto& s : socks) { pdebug("%d ", s.sock_fd); } pdebug("\n");

		memset(sock_buf, 0, sizeof(sock_buf));

		while (true) {
			// 初期化と使用するソケットのシステム側への通知
			FD_ZERO(&rfds);
			FD_SET(sock_tun, &rfds);
			for (const auto& s : socks) { FD_SET(s.sock_fd, &rfds); }

			// データを受信するまで待機
			if (select(max_fd + 1, &rfds, NULL, NULL, NULL) < 0) {
				if (errno == EINTR) continue;

				perror("select()");
				print_error("errno = %d\n", errno);
				return false;
			}
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
		return true;
	}
} // namespace client
