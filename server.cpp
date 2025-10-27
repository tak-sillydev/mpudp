#include <algorithm>

#include "mpudp.h"

// addrは無視される
bool MPUDPTunnelServer::Start(const std::string& tun_name, const std::string& addr, const int port) {
	sockaddr_in	local_addr;
	socklen_t	szaddr = sizeof(local_addr);
	int			optval = 1;

	if (!this->SetTunDevice(tun_name.c_str())) { return false; }

	// ソケットの作成とオプションの設定
	if ((this->sock_recv = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("socket()");
		print_error("Couldn't create socket\n");
		print_error("errno = %d\n", errno);
		return false;
	}
	if (setsockopt(this->sock_recv, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
		perror("setsockopt()");
		print_error("Couldn't set value of SO_REUSEASSR\n");
		print_error("errno = %d\n", errno);
		return false;
	}
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin_family		= AF_INET;
	local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
	local_addr.sin_port			= htons(port);

	if (bind(this->sock_recv, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
		perror("bind()");
		print_error("errno = %d\n", errno);
		return false;
	}
	getsockname(this->sock_recv, (sockaddr*)&local_addr, &szaddr);
	pdebug("eth: addr: %s, port: %d\n",
		inet_ntoa(local_addr.sin_addr),
		ntohs(local_addr.sin_port)
	);
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
void MPUDPTunnelServer::_RefreshConnection(sockaddr_in& addr_from) {
	using namespace std::chrono;

	TUN_HEADER	*phead = this->GetHeader();

	// 接続リストに今回の接続のデバイスIDで検索をかける
	auto conn_it = std::find_if(connection_list.begin(), connection_list.end(),
		[phead](const CONNECTIONS& c) { return phead->device_id == c.device_id; }
	);
	// 過去に接続されたデバイスからのデータか？
	if (conn_it == connection_list.end()) {
		pdebug("new routes\n");
		CONNECTIONS	c;
		SOCKET_PACK	s;

		s.sock_fd = this->sock_recv;
		s.remote_addr = addr_from;
		this->socks.emplace_back(std::move(s));

		c.addr = addr_from;
		c.device_id = phead->device_id;
		c.connected_time = system_clock::now();
		this->connection_list.emplace_back(c);
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
				sock_it->remote_addr = addr_from;
			}
		}
		// 接続時間の更新
		conn_it->connected_time = system_clock::now();
	}
	// 最後に「１分以上データの飛んでこない接続元」を閉じないといけない
	// socksをリストにした方が良いかも
	// sock_recv のclose問題は、先にsock_fd = -1 としておけばよさそう（-1のときはcloseしない）

	// 削除対象のコネクションを列挙
	auto now = system_clock::now();
	auto conn_end = std::remove_if(connection_list.begin(), connection_list.end(),
		[now](const CONNECTIONS& c) {
			return duration_cast<minutes>(now - c.connected_time).count() >= 1;
		}
	);
	if (conn_end == connection_list.end()) return;

	auto sock_end = std::remove_if(socks.begin(), socks.end(),
		[&](const SOCKET_PACK& s) {
			// それぞれの s.remote_addr に対して
			// 削除対象のコネクションを順に照合し、合致するものがあれば true（削除）
			auto it = std::find_if(conn_end, connection_list.end(),
				[&](const CONNECTIONS& c) { return is_same_addr(c.addr, s.remote_addr); }
			);
			return it != connection_list.end();
		}
 	);
	pdebug("REMOVED connections : device_id = ");
	std::for_each(conn_end, connection_list.end(), [](const auto& c){ pdebug("%d ", c.device_id); });
	pdebug("\n");
	pdebug("REMOVED SOCKS : addr = ");
	std::for_each(sock_end, socks.end(), [](const auto& s){
		pdebug("%s:%d ", inet_ntoa(s.remote_addr.sin_addr), ntohs(s.remote_addr.sin_port));
	});
	pdebug("\n");

	connection_list.erase(conn_end, connection_list.end());
	if (sock_end != socks.end()) {
		std::for_each(sock_end, socks.end(), [](SOCKET_PACK& s){ s.sock_fd = -1; });
		socks.erase(sock_end, socks.end());
	}
	return;
}

void MPUDPTunnelServer::_process_echo_packet() {
	ECHO_PACKET	*echo = (ECHO_PACKET*)this->GetHeader();

	pdebug("device_id = %d, seq = %d, time = %ld\n", echo->tun_header.device_id, echo->seq, echo->tm_start);
	this->SendToAllDevices(sizeof(ECHO_PACKET) - sizeof(TUN_HEADER), PACKET_TYPE_MANAGE);
	return;
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
	int		tun_seq = 0;

	auto	phead = this->GetHeader();
	auto	pdata = this->GetDataPtr();

	max_fd = max(sock_tun, sock_recv);

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
				std::lock_guard<std::mutex>	lock(buf_mtx);

				nread = tun_eread(this->sock_tun, (void*)pdata, BUFSIZE);
				pdebug_tunrecv(tun_seq, nread, pdata);
				tun_seq++;

				// それぞれのソケットリストに書かれたアドレスへパケットを送信
				nwrite = this->SendToAllDevices(nread, PACKET_TYPE_GENERAL);
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
				std::lock_guard<std::mutex>		lock(buf_mtx);
				sockaddr_in	addr_from;

				nread = this->RecvFrom(&addr_from);
				this->_RefreshConnection(addr_from);

				pdebug_ethrecv(phead->seq, nread, (uint8_t*)phead, addr_from);

				if (phead->type == PACKET_TYPE_MANAGE) {
					switch (pdata[0]) {
					case MANAGE_TYPE_ECHO:
						this->_process_echo_packet();
						pdebug("packet was ECHO packet: replied to eth\n");
						break;
					}
				}
				else {
					nwrite = tun_ewrite(sock_tun, pdata, phead->length);
					pdebug("packet was sent to tun seq=%d : write %lu bytes\n", phead->seq, nwrite);
				}	
			}
			catch (std::exception &e) {
				perror("recvfrom / ewrite");
				pdebug("errno = %d\n", errno);
				print_error("%s - the data will be discarded. Continue.\n", e.what());
			}
		}
	}
	return true;
}