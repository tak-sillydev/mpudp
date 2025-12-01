#include <algorithm>
#include <limits>

#include "mpudp.h"

bool MPUDPTunnelServer::_SetupSocket(int& sock_fd, int listen_port) {
	sockaddr_in	listen_addr;
	socklen_t	addr_len = sizeof(listen_addr);
	int		optval = 1;

	if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("socket()");
		print_error("Couldn't create socket\n");
		print_error("errno = %d\n", errno);
		return false;
	}
	if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
		perror("setsockopt()");
		print_error("Couldn't set value of SO_REUSEASSR\n");
		print_error("errno = %d\n", errno);
		return false;
	}
	memset(&listen_addr, 0, sizeof(listen_addr));
	listen_addr.sin_family		= AF_INET;
	listen_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
	listen_addr.sin_port		= htons(listen_port);

	if (bind(sock_fd, (sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
		perror("bind()");
		print_error("errno = %d\n", errno);
		return false;
	}
	getsockname(sock_fd, (sockaddr*)&listen_addr, &addr_len);
	pdebug("eth: addr: %s, port: %d\n",
		inet_ntoa(listen_addr.sin_addr),
		ntohs(listen_addr.sin_port)
	);
	return true;
}

// addrは無視される
bool MPUDPTunnelServer::Start(const std::string& tun_name, const int port) {
	if (!this->SetTunDevice(tun_name.c_str())) { return false; }

	// ソケットの作成とオプションの設定
	if (!this->_SetupSocket(this->sock_recv, port)) { return false; }

	return true;
}

ssize_t MPUDPTunnelServer::RecvFrom(sockaddr_in *addr_from) {
	TUN_HEADER	*phead = (TUN_HEADER*)this->GetHeader();
	sockaddr_in	addr;
	socklen_t	addr_len = sizeof(addr);
	ssize_t		nread = -1;

	nread = recvfrom(
		sock_recv, phead, sizeof(TUN_HEADER),
		MSG_PEEK, (sockaddr*)&addr, &addr_len
	);
	if (nread < 0) {
		throw std::runtime_error("recvfrom returned an invalid value");
	}

	// パケット分割に備え、対向側が TUN_HEADER に書き込んだデータ長を読み切るまで待機
	nread = recvfrom(
		sock_recv, phead, sizeof(TUN_HEADER) + phead->length,
		MSG_WAITALL, (sockaddr*)&addr, &addr_len
	);
	if (nread < 0) {
		throw std::runtime_error("recvfrom returned an invalid value");
	}
	if (addr_from != nullptr) {
		*addr_from = addr;
	}
	return nread;
}

// 今までにない経路からの通信なら返信リストに登録
// デバイスIDが同じでも、ポート番号などアドレス情報が変わっていれば更新
void MPUDPTunnelServer::_RefreshConnection(sockaddr_in& addr_from, int nrecv) {
	TUN_HEADER	*phead = this->GetHeader();

	// 接続リストに今回の接続のデバイスIDで検索をかける
	auto conn_it = std::find_if(conn_list.begin(), conn_list.end(),
		[phead](const CONNECTIONS& c) { return phead->device_id == c.device_id; }
	);
	// 過去に接続されたデバイスからのデータか？
	if (conn_it == conn_list.end()) {
		pdebug("new routes\n");
		CONNECTIONS	c;
		SOCKET_PACK	s;

		s.sock_fd     = this->sock_recv;
		s.device_id   = phead->device_id;
		s.remote_addr = addr_from;
		this->socks.emplace_back(std::move(s));

		c.addr = addr_from;
		c.device_id = phead->device_id;
		c.connected_time = chr::system_clock::now();

		this->conn_list.emplace_back(c);	// 末尾に追加して
		conn_it = --conn_list.end();		// そのイテレータを取る
		conn_it->reset_stats();
	}
	else {
		// 同じデバイスIDからの接続
		if (!is_same_addr(conn_it->addr, addr_from)) {
			// これまでとは異なる経路からの接続
			sockaddr_in	old_addr = conn_it->addr;

			conn_it->addr = addr_from;

			auto sock_it = std::find_if(socks.begin(), socks.end(),
				[old_addr](const SOCKET_PACK& s) { return is_same_addr(old_addr, s.remote_addr); }
			);
			if (sock_it != socks.end()) {
				sock_it->remote_addr = addr_from;	// 接続経路の更新
			}
		}
		// 接続時間の更新
		conn_it->connected_time = chr::system_clock::now();
	}

	// 回線速度とパケットロス率の測定
	if (conn_it->last_seq + 1 < phead->seq_dev) {
		// シーケンスが飛んでいる・パケロスもしくは順序交代
		for (uint32_t q = conn_it->last_seq + 1; q < phead->seq_dev; q++, conn_it->loss_packets++) {
			conn_it->loss_buf.push(q);	// ロスパケットリストに追加
		}
	}
	else if (phead->seq_dev < conn_it->last_seq) {
		// 順序交代したパケットが届いた
		auto q = std::find(conn_it->loss_buf.begin(), conn_it->loss_buf.end(), phead->seq_dev);

		// reset_stat 直後（もうロスしたものとして計上してしまった）に順序交代したパケットが届くと困る
		if (q != conn_it->loss_buf.end()) {
			*q = -1;	// ロスパケリストから削除
			conn_it->loss_packets--;
		}
	}
	conn_it->last_seq = max(conn_it->last_seq, phead->seq_dev);
	conn_it->rcvd_bytes += nrecv;
	conn_it->rcvd_packets++;

	pdebug(
		"speed: %dB, rcvd = %d, loss = %d, loss.rate = %f\n",
		conn_it->rcvd_bytes, conn_it->rcvd_packets,
		conn_it->loss_packets,
		(double)conn_it->loss_packets / (conn_it->rcvd_packets + conn_it->loss_packets)
	);
	// 最後に「１分以上データの飛んでこない接続元」を閉じる
	// socksをリストにした方が良いかも

	// 削除対象のコネクションを探す
	auto now = chr::system_clock::now();
	auto conn_end = std::remove_if(conn_list.begin(), conn_list.end(),
		[now](const CONNECTIONS& c) {
			return chr::duration_cast<chr::minutes>(now - c.connected_time).count() >= 1;
		}
	);
	if (conn_end == conn_list.end()) return;	// 削除すべきものはない

	// 同様に、削除すべき SOCKET_PACK を上で集めた「削除対象」と照合して探す
	auto sock_end = std::remove_if(socks.begin(), socks.end(),
		[&](const SOCKET_PACK& s) {
			// それぞれの s.remote_addr に対して
			// 削除対象のコネクションを順に照合し、合致するものがあれば true（削除）を返す
			auto it = std::find_if(conn_end, conn_list.end(),
				[&](const CONNECTIONS& c) { return is_same_addr(c.addr, s.remote_addr); }
			);
			return it != conn_list.end();
		}
 	);
	pdebug("REMOVED connections : device_id = ");
	std::for_each(conn_end, conn_list.end(), [](const auto& c){ pdebug("%lx ", c.device_id); });
	pdebug("\n");
	pdebug("REMOVED sockets : addr = ");
	std::for_each(sock_end, socks.end(), [](const auto& s){
		pdebug("%s:%d ", inet_ntoa(s.remote_addr.sin_addr), ntohs(s.remote_addr.sin_port));
	});
	pdebug("\n");

	conn_list.erase(conn_end, conn_list.end());
	if (sock_end != socks.end()) {
		std::for_each(sock_end, socks.end(), [](SOCKET_PACK& s){ s.sock_fd = -1; }); // close 避け
		socks.erase(sock_end, socks.end());
	}
	return;
}

bool MPUDPTunnelServer::_ProcessManagementPacket() {
	char	*sign = (char*)this->GetDataPtr();

	if (strncmp(sign, SIGNATURE_ECHO, strlen(SIGNATURE_ECHO)) == 0){
		ECHO_PACKET	*buf = (ECHO_PACKET*)this->GetHeader();

		pdebug(
			"ECHO: device_id = %lx, seq = %d, tm = %lu\n",
			buf->header.device_id, buf->seq, buf->tm_start
		);
		// 送られてきたデータをそのまま送り返す
		this->SendToAllDevices(sizeof(ECHO_PACKET) - sizeof(TUN_HEADER), TYPE_MANAGEMENT);
	}
	return true;
}

bool MPUDPTunnelServer::_SendStatsPacket() {
	STAT_PACKET	*buf = (STAT_PACKET*)this->GetHeader();

	buf->set_signature();

	for (auto& c : conn_list) {
		buf->loss_packets = c.loss_packets;
		buf->rcvd_packets = c.rcvd_packets;
		buf->rcvd_bytes   = c.rcvd_bytes;
		buf->device_id	  = c.device_id;

		this->SendToAllDevices(sizeof(STAT_PACKET) - sizeof(TUN_HEADER), TYPE_MANAGEMENT);
		c.reset_stats();
	}
	return true;
}

/*
 * サーバーモード
 * サーバーモードでは、ソケットリストは経路情報だけを格納するものとして用い、
 * データの送受信には用いない（代わりに待ち受けソケットを用いる）
 * 転送モードはSTABLE、受信側で stable_id を確認して重複したものは破棄する
 */
bool MPUDPTunnelServer::MainLoop() {
	fd_set	rfds;
	int		max_fd;

	int		nread, nwrite;
	int		sel;

	auto	phead = this->GetHeader();
	auto	pdata = this->GetDataPtr();

	chr::system_clock::time_point	nw;
	timeval	tv = { 0, 1 * 1000 * 100 };

	max_fd = max(sock_tun, sock_recv);

	while (true) {
		FD_ZERO(&rfds);
		FD_SET(sock_tun,  &rfds);
		FD_SET(sock_recv, &rfds);

		// データ到着まで待機
		sel = select(max_fd + 1, &rfds, NULL, NULL, &tv);
		if (sel < 0) {
			if (errno == EINTR) continue;

			perror("select()");
			print_error("errno = %d\n", errno);
			return false;
		}
		if (sel == 0) {
			// タイムアウト
			// 統計情報を送る
			this->_SendStatsPacket();

			tv.tv_sec  = 1;
			tv.tv_usec = 0;
			nw = chr::system_clock::now();
			continue;
		}
		else {
			if (FD_ISSET(sock_tun, &rfds)) {
				try {
					pdebug("\n===== TUN DEVICE RECEIVED DATA =====\n");
					nread = tun_eread(this->sock_tun, (void*)pdata, BUFSIZE);
					pdebug_tunrecv(nread, pdata);

					// それぞれのソケットリストに書かれたアドレスへパケットを送信
					nwrite = this->SendToAllDevices(nread);
					if (nwrite == 0) {
						pdebug("No connection exists\n");
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
				try {
					pdebug("\n===== ETH DEVICE RECEIVED DATA =====\n");
					sockaddr_in	addr_from;

					nread = this->RecvFrom(&addr_from);

					if (nread > 0) {
						this->_RefreshConnection(addr_from, nread);

						pdebug_ethrecv(phead->seq_all, nread, (uint8_t*)phead, addr_from);

						if (phead->type == TYPE_MANAGEMENT) {
							this->_ProcessManagementPacket();
						}
						else {
							nwrite = tun_ewrite(sock_tun, pdata, phead->length);
							pdebug("packet was sent to tun seq=%d : write %lu bytes\n", phead->seq_all, nwrite);
						}
					}
				}
				catch (std::exception &e) {
					perror("recvfrom / ewrite");
					pdebug("errno = %d\n", errno);
					print_error("%s - the data will be discarded. Continue.\n", e.what());
				}
			}
			settimer_1sec(tv, nw);
		}
	}
	return true;
}