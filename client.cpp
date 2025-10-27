#include <algorithm>

#include <sys/types.h>
#include <netdb.h>

#include "ringbuf.h"
#include "print.h"
#include "mpudp.h"

bool MPUDPTunnelClient::_GetAddressInfo(const std::string& dst_addr, const int dst_port, addrinfo **result) {
	char		port_str[16];
	addrinfo	hints;
	int			err;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family		= AF_INET;
	hints.ai_socktype	= SOCK_DGRAM;

	sprintf(port_str, "%hu", dst_port);
	if ((err = getaddrinfo(dst_addr.c_str(), port_str, &hints, result)) < 0) {
		perror("getaddrinfo()");
		print_error("err = %d, reason = %s\n", err, gai_strerror(err));
		print_error("errno = %d\n", errno);
		return false;
	}
	return true;
}

bool MPUDPTunnelClient::Start(const std::string& tun_name, const std::string& addr, const int port) {
	addrinfo	*ai;
	socklen_t	szaddr;
	int			optval = 1;

	if (!this->SetTunDevice(tun_name.c_str())) { return false; }
	if (!this->_GetAddressInfo(addr, port, &ai)) { return false; }

	szaddr = ai->ai_addrlen;

	for (auto& s : this->socks) {
		// 使用する実デバイスにソケットを割り当てる
		// ソケットの作成とオプションの設定

		if ((s.sock_fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) < 0) {
			perror("socket()");
			print_error("Couldn't create socket - %s\n", s.eth_name.c_str());
			print_error("errno = %d\n", errno);
			return false;
		}
		if (setsockopt(s.sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_REUSEASSR - %s\n", s.eth_name.c_str());
			print_error("errno = %d\n", errno);
			return false;
		}
		if (setsockopt(s.sock_fd, SOL_SOCKET, SO_BINDTODEVICE, s.eth_name.c_str(), s.eth_name.length()) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_BINDTODEVICE - %s\n", s.eth_name.c_str());
			print_error("errno = %d\n", errno);
			return false;
		}
		// 接続先の設定
		if (connect(s.sock_fd, ai->ai_addr, ai->ai_addrlen) < 0) {
			perror("connect()");
			print_error("device = %s, errno = %d\n", s.eth_name.c_str(), errno);
			return false;
		}
		s.remote_addr = *(sockaddr_in *)ai->ai_addr;

		// ローカル側で使用するポートの割当
		s.local_addr.sin_family			= ai->ai_family;
		s.local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
		s.local_addr.sin_port			= htons(0);
		bind(s.sock_fd, (sockaddr*)&(s.local_addr), sizeof(s.local_addr));
		getsockname(s.sock_fd, (sockaddr*)&(s.local_addr), &szaddr);	// bind() によって使用ポートが割り当てられたので情報を取得

		pdebug("eth[%s]: fd: %d, local addr: %s, port: %d\n",
			s.eth_name.c_str(),
			s.sock_fd,
			inet_ntoa(s.local_addr.sin_addr),
			ntohs(s.local_addr.sin_port)
		);
	}
	return true;
}

void MPUDPTunnelClient::AddDevice(const std::string& device_name) {
	SOCKET_PACK	s;

	s.eth_name = device_name;
	this->socks.emplace_back(std::move(s));
	return;
}

// １秒おきに各ソケットにECHOパケットを流す
std::unique_ptr<std::thread> MPUDPTunnelClient::_send_echo_thread() {
	return std::unique_ptr<std::thread>(new std::thread([this](){
		ECHO_PACKET	*echo = (ECHO_PACKET*)this->GetHeader();
		uint32_t echo_seq = 0;

		while (true) {
			for (auto& s : this->socks) {
				std::lock_guard<std::mutex>	lock(this->buf_mtx);

				echo->type = MANAGE_TYPE_ECHO;
				echo->seq  = echo_seq;
				echo->tm_start = system_clock::now();

				SendTo(s, sizeof(ECHO_PACKET) - sizeof(TUN_HEADER), PACKET_TYPE_MANAGE);
				echo_seq++;
			} // mutex は自動解放
			sleep(1);
		}
	}));
}

void MPUDPTunnelClient::_process_echo_packet(const ECHO_PACKET *echo) {
	using namespace std::chrono;

	auto diff_us = duration_cast<microseconds>(system_clock::now() - echo->tm_start);

	pdebug(
		"THIS IS ECHO PACKET: device_id = %d, seq = %d, RTT = %d.%dms\n",
		echo->tun_header.device_id, echo->seq, diff_us / 1000, diff_us % 1000
	);
	return;
}

/*
 * クライアントモード送受ループ
 * クライアントモードモードでは、socks の各要素はそれぞれの eth デバイスに割り当てられたソケット
 * イテレータを走査してデータを受信する
 */
bool MPUDPTunnelClient::MainLoop() {
	fd_set	rfds;
	int		max_fd = -1;

	auto socks_it = socks.begin();
	auto phead = this->GetHeader();
	auto pdata = this->GetDataPtr();

	uint32_t	nread, nwrite;
	uint32_t	tun_seq = 0;

	/*
	 * 受信済みのパケット番号を記録する場所
	 * MODE_STABLE で送信されたパケットは全部の経路に同じものを流して冗長化するので、
	 * 受信側で「すでに受信した」パケットは廃棄する必要がある。
	 * このバッファは溢れた場合、古いものから自動的に削除される仕組み。
	 */
	ringbuf<decltype(GetHeader()->seq), 32>	seq_rec(-1);

	std::for_each(
		socks.begin(), socks.end(),
		[&max_fd](const SOCKET_PACK& x){ max_fd = max(x.sock_fd, max_fd); }
	);
	max_fd = max(this->sock_tun, max_fd);

	auto t = this->_send_echo_thread();

	while (true) {

		// 初期化と使用するソケットのシステム側への通知
		FD_ZERO(&rfds);
		FD_SET(this->sock_tun, &rfds);
		for (const auto& s : this->socks) { FD_SET(s.sock_fd, &rfds); }

		// データを受信するまで待機
		if (select(max_fd + 1, &rfds, NULL, NULL, NULL) < 0) {
			if (errno == EINTR) continue;

			perror("select()");
			print_error("errno = %d\n", errno);
			return false;
		}
		if (FD_ISSET(this->sock_tun, &rfds)) {
			/* 
			 * TUN デバイス側からデータを受信
			 * ここに書き込まれるデータは生のIPパケット
			 * ETH デバイスを選定してデータを書き込む（ネットワーク側に流す）
			 */
			try {
				pdebug("\n===== TUN DEVICE RECEIVED DATA =====\n");
				std::lock_guard<std::mutex>	lock(buf_mtx);	// try の間はバッファを占有

				// this->data_buf にデータを書き込んでおくと勝手に運んでくれる
				nread = tun_eread(sock_tun, pdata, BUFSIZE);
				pdebug_tunrecv(tun_seq, nread, pdata);
				tun_seq++;

				// ラウンドロビンでデータを送る
				// ここにパケットを効率よく分散する機構を組み込む
				nwrite = this->SendTo(*socks_it, nread, PACKET_TYPE_GENERAL);
				pdebug("packet was sent to eth device = %s: %lu bytes\n", socks_it->eth_name.c_str(), nwrite);

				socks_it++;
				if (socks_it == socks.cend()) { socks_it = socks.begin(); }
			} catch (std::exception& e) {
				perror("eread / sendto");
				print_error("errno = %d\n", errno);
				print_error("%s - the data will be discarded. Continue.\n", e.what());
			}
		}
		for (auto& s : this->socks) {
			if (FD_ISSET(s.sock_fd, &rfds)) {
				/* 
				 * ETH デバイス側からデータを受信
				 * パケットサイズがMTUを超える場合、パケットは複数に分割される
				 * この分割、また受信時の再合成の処理はより低いレイヤー（ネットワーク層）で行われるので、
				 * UDPのレイヤでは特に考えなくて良い。ただし、パケットが遅れて到着する可能性はあるので、
				 * 送信したバイト数を読み切るまで待機する処理が必要（ここでは readn が行う）
				 */
				try {
					pdebug("\n===== ETH DEVICE [%s] RECEIVED DATA =====\n", s.eth_name.c_str());
					std::lock_guard<std::mutex>	lock(buf_mtx);	// try の間はバッファを占有
					sockaddr_in	addr_from;

					nread = this->RecvFrom(s, &addr_from);

					pdebug_ethrecv(phead->seq, nread, (uint8_t*)phead, addr_from);

					if (phead->mode == MODE_STABLE) {
						const auto it = std::find(seq_rec.begin(), seq_rec.end(), phead->seq);

						pdebug("seq = %d\n", phead->seq);

						if (it != seq_rec.end()) {
							pdebug("packet was already received: skip.\n");
							continue;
						}
						if (phead->type == PACKET_TYPE_MANAGE) {
							switch(pdata[0]) {
							case MANAGE_TYPE_ECHO:
								this->_process_echo_packet((ECHO_PACKET*)phead);
								break;
							}
							seq_rec.push(phead->seq);
							continue;
						}
						seq_rec.push(phead->seq);
					}
					nwrite = tun_ewrite(sock_tun, pdata, phead->length);
					pdebug("packet was sent to tun seq=%d : write %lu bytes\n", phead->seq, nwrite);
				}
				catch (std::exception &e) {
					perror("recvfrom / ewrite");
					pdebug("errno = %d\n", errno);
					print_error("%s - the data will be discarded. Continue.\n", e.what());
				}
			}
		}
	}
	t->join();
	return true;
}
