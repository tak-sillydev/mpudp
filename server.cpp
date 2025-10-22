#include <algorithm>
#include <stdexcept>

#include "print.h"
#include "server.h"

namespace server {
	bool setup(int& sock_recv, const int stby_port) {
		sockaddr_in	local_addr;
		socklen_t	szaddr = sizeof(sockaddr_in);
		int			optval = 1;

		// ソケットの作成とオプションの設定
		if ((sock_recv = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			perror("socket()");
			print_error("Couldn't create socket\n");
			print_error("errno = %d\n", errno);
			return false;
		}
		if (setsockopt(sock_recv, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_REUSEASSR\n");
			print_error("errno = %d\n", errno);
			return false;
		}
		memset(&local_addr, 0, sizeof(local_addr));
		local_addr.sin_family		= AF_INET;
		local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
		local_addr.sin_port			= htons(stby_port);

		if (bind(sock_recv, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
			perror("bind()");
			print_error("errno = %d\n", errno);
			return false;
		}
		getsockname(sock_recv, (sockaddr*)&local_addr, &szaddr);
		pdebug("eth: addr: %s, port: %d\n",
			inet_ntoa(local_addr.sin_addr),
			ntohs(local_addr.sin_port)
		);
		return true;
	}

	/*
	 * サーバーモード
	 * サーバーモードでは、ソケットリストは経路情報だけを格納するものとして用い、
	 * データの送受信には用いない（代わりに待ち受けソケットを用いる）
	 * 転送モードはSTABLE、受信側で stable_id を確認して重複したものは破棄する
	 */
	bool main_loop(int sock_tun, int sock_recv, std::vector<SOCKET_PACK>& socks) {
		fd_set	rfds;
		int		max_fd;

		int		nread, nwrite;
		int		tun_seq = 0, eth_seq = 0, stable_id = 0;

		// 送受信に用いるバッファ
		uint8_t		sock_buf[sizeof(TUN_HEADER) + BUFSIZE];
		TUN_HEADER	*pbuf_head = (TUN_HEADER*)sock_buf;
		uint8_t		*pbuf_data = sock_buf + sizeof(TUN_HEADER);

		max_fd = max(sock_tun, sock_recv);
		memset(sock_buf, 0, sizeof(sock_buf));

		while (true) {
			FD_ZERO(&rfds);
			FD_SET(sock_tun,  &rfds);
			FD_SET(sock_recv, &rfds);

			// データ到着まで待機
			if (select(max_fd + 1, &rfds, NULL, NULL, NULL) < 0) {
				if (errno == EINTR) continue;

				perror("select()");
				print_error("errno = %d\n", errno);
				return false;
			}
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
		return true;
	}
} // namespace server 
