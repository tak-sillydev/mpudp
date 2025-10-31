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

bool MPUDPTunnelClient::_SetupSocket(int& sock_fd, const addrinfo& ai, const std::string& eth_name) {
	int	optval = 1;

	if ((sock_fd = socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol)) < 0) {
		perror("socket()");
		print_error("Couldn't create socket - %s\n", eth_name.c_str());
		print_error("errno = %d\n", errno);
		return false;
	}
	if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
		perror("setsockopt()");
		print_error("Couldn't set value of SO_REUSEASSR - %s\n", eth_name.c_str());
		print_error("errno = %d\n", errno);
		return false;
	}
	if (setsockopt(sock_fd, SOL_SOCKET, SO_BINDTODEVICE, eth_name.c_str(), eth_name.length()) < 0) {
		perror("setsockopt()");
		print_error("Couldn't set value of SO_BINDTODEVICE - %s\n", eth_name.c_str());
		print_error("errno = %d\n", errno);
		return false;
	}
	// 接続先の設定
	if (connect(sock_fd, ai.ai_addr, ai.ai_addrlen) < 0) {
		perror("connect()");
		print_error("device = %s, errno = %d\n", eth_name.c_str(), errno);
		return false;
	}
	return true;
}

bool MPUDPTunnelClient::Start(const std::string& tun_name, const std::string& addr, const int port) {
	addrinfo	*ai;
	socklen_t	szaddr;

	if (!this->SetTunDevice(tun_name.c_str())) { return false; }
	if (!this->_GetAddressInfo(addr, port, &ai)) { return false; }

	szaddr = ai->ai_addrlen;

	for (auto& s : this->socks) {
		// 使用する実デバイスにソケットを割り当てる
		// ソケットの作成とオプションの設定
		if (!this->_SetupSocket(s.sock_fd, *ai, s.eth_name)) {
			freeaddrinfo(ai);
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
	this->th_echo = this->_StartEchoThread(addr);
	freeaddrinfo(ai);
	return true;
}

void MPUDPTunnelClient::AddDevice(const std::string& device_name) {
	SOCKET_PACK	s;

	s.eth_name = device_name;
	this->socks.emplace_back(std::move(s));
	return;
}

// １秒おきに各ソケットにECHOパケットを流す（およそラムダ関数で処理する長さではない）
// dst_addr はコピーの方がよい。スレッド間では時間の流れが違うので別スレッドの dst_addr の値を保証できないから。
// this（インスタンスのアドレスを指す）はプログラム終了まで同じはずなので（コピーとかしない限り）そのままでよい。
std::unique_ptr<std::thread> MPUDPTunnelClient::_StartEchoThread(const std::string& dst_addr) {
#define	perror_th(s)				perror("[ECHO_THREAD] " s)
#define	print_error_th(format, ...)	print_error(("[ECHO_THREAD] " format), ## __VA_ARGS__)
#define	pdebug_th(format, ...)		pdebug("[ECHO_THREAD] " format, ## __VA_ARGS__)

	return std::unique_ptr<std::thread>(new std::thread([dst_addr, this](){
		using namespace std::chrono;

		std::unique_ptr<ECHO_PACKET>	buf(new ECHO_PACKET);
		std::vector<ECHO_SOCKETS>		echo_socks((std::size_t)this->socks.size());

		ringbuf<decltype(buf->header.seq),32>	already_recvd_seq(-1);

		addrinfo	*ai;
		sockaddr_in	addr;
		ssize_t		n;
		int64_t		echo_seq = 0;
		socklen_t	addr_len = sizeof(addr);

		timeval	tv = { 0, 1000 };	// 最初は1ミリ秒後に強制的にタイムアウトさせてECHOを送る
		fd_set	rfds;
		int		max_fd = -1;
		int		selret;

		system_clock::time_point	now_time;

		this->_GetAddressInfo(dst_addr, PORT_PING, &ai);

		for (size_t i = 0; i < this->socks.size(); i++) {
			// if false == 親スレッドに異常通知、終了
			this->_SetupSocket(echo_socks[i].echo_sock, *ai, socks[i].eth_name);
			echo_socks[i].device_id = socks[i].sock_fd;
			max_fd = max(max_fd, echo_socks[i].echo_sock);
			pdebug_th("echo_sockfd = %d, sock_fd = %d\n", echo_socks[i].echo_sock, socks[i].sock_fd);

			echo_socks[i].recvd_count = 0;
			echo_socks[i].rtt_avg = microseconds().min();
			echo_socks[i].rtt_max = microseconds().min();
			echo_socks[i].status.fill({ system_clock::now(), -1 });
		}
		while (true) {
			FD_ZERO(&rfds);
			for (auto& e : echo_socks) { FD_SET(e.echo_sock, &rfds); }

			selret = select(max_fd + 1, &rfds, NULL, NULL, &tv);

			if (selret < 0) {
				// 異常発生
				if (errno == EINTR) continue;

				perror_th("select : ");
				exit(1);
			}
			if (selret == 0) {
				// タイムアウト
				for (auto& e : echo_socks) {
					buf->header.device_id = e.device_id;
					buf->header.seq = echo_seq;
					buf->tm_start = system_clock::now();

					n = sendto(e.echo_sock, buf.get(), sizeof(ECHO_PACKET), 0, ai->ai_addr, sizeof(*ai->ai_addr));

					if (n < 0) {
						perror_th("sendto : ");
						// 回線落ち、パケットの振り替え処理へ
						continue;
					}
					std::for_each(e.status.begin(), e.status.end(),
						[&already_recvd_seq, &e](CONNECT_STATUS& s) {
							// タイムアウトを 950ms に設定（selectが１秒でタイムアウトしたときに確実に真にするため）
							// 送信したパケットのタイムアウトを監視
							if (s.seq != -1 &&
								duration_cast<milliseconds>(system_clock::now() - s.ping_sent_time).count() >= PING_TIMEOUT_MSEC) {
								pdebug_th(
									"ECHO PACKET TIMEOUT : sock_fd = %d, "
									"device_id = %d, seq = %d\n",
									e.echo_sock, e.device_id, s.seq
								);
								already_recvd_seq.push(s.seq);
								s.ping_sent_time = system_clock::time_point().min();	// オーバーヘッドありそうなんだけど…
								s.seq = -1;
							}
						}
					);
					e.status.push({ buf->tm_start, echo_seq });
					echo_seq++;
				}
				now_time = system_clock::now();
				tv.tv_sec  = 1;
				tv.tv_usec = 0;
				continue;
			}
			for (auto& e : echo_socks) {
				if (FD_ISSET(e.echo_sock, &rfds)) {
					n = recvfrom(
						e.echo_sock, buf.get(), sizeof(ECHO_PACKET),
						MSG_WAITALL, (sockaddr*)&addr, &addr_len
					);
					if (n < 0) {
						perror_th("recvfrom : ");
						// errno == CONNECTION_REFUSED ?
						// 回線落ち、パケット振り替え処理へ
						continue;
					}
					if (strncmp(buf->header.signature, SIGNATURE_MANAGEMENT, sizeof(buf->header.signature)) != 0) {
						pdebug_th("signature is not valid\n");
						continue;
					}
					if (strncmp(buf->signature, SIGNATURE_ECHO, sizeof(buf->signature)) != 0) {
						pdebug_th("signature is not valid\n");
						continue;
					}
					const auto ars_it = std::find(already_recvd_seq.begin(), already_recvd_seq.end(), buf->header.seq);
					if (ars_it != already_recvd_seq.end()) {
						pdebug_th(
							"sock_fd = %d, device_id = %d, seq = %d, "
							"packet is already received. skip.\n",
							e.echo_sock, buf->header.device_id, buf->header.seq
						);
						continue;
					}
					// 行きと帰りではパケットの経路が異なる
					// d is device
					const auto d = std::find_if(echo_socks.begin(), echo_socks.end(),
						[&buf](const ECHO_SOCKETS& e) { return e.device_id == buf->header.device_id; }
					);
					if (d == echo_socks.end()) { continue; }

					auto diff_us = duration_cast<microseconds>(system_clock::now() - buf->tm_start);

					d->rtt_max = max(d->rtt_max, diff_us);
					d->rtt_avg = (d->rtt_avg * d->recvd_count + diff_us) / (d->recvd_count + 1);
					d->score = diff_us.count() / (double)d->rtt_max.count();
					d->recvd_count++;
					already_recvd_seq.push(buf->header.seq);

					const auto sts_it = std::find_if(d->status.begin(), d->status.end(),
						[&buf](const CONNECT_STATUS& c) { return c.seq == buf->header.seq; }
					);
					if (sts_it != d->status.end()) {
						sts_it->ping_sent_time = system_clock::now();
						sts_it->seq = -1;
					}
					pdebug_th(
						"ECHO PACKET RECVD: sock_fd = %d, device_id = %d, seq = %d, "
						"RTT = %d.%dms, max = %d.%dms, avg = %d.%dms, score = %.3f\n",
						e.echo_sock, buf->header.device_id, buf->header.seq,
						  diff_us.count() / 1000,   diff_us.count() % 1000,
						d->rtt_max.count() / 1000, d->rtt_max.count() % 1000,
						d->rtt_avg.count() / 1000, d->rtt_avg.count() % 1000,
						d->score
					);
				}
			}
			auto usec = duration_cast<microseconds>(system_clock::now() - now_time).count();
			tv.tv_sec  = 0;
			tv.tv_usec = (suseconds_t)(1 * 1000 * 1000 - usec);
		}
		freeaddrinfo(ai);
	}));
#undef	perror_th
#undef	print_error_th
#undef	pdebug_th
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
	ringbuf<decltype(GetHeader()->seq_all), 32>	seq_rec(-1);

	std::for_each(
		socks.begin(), socks.end(),
		[&max_fd](const SOCKET_PACK& x){ max_fd = max(x.sock_fd, max_fd); }
	);
	max_fd = max(this->sock_tun, max_fd);

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
				nwrite = this->SendTo(*socks_it, nread);
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

					pdebug_ethrecv(phead->seq_all, nread, (uint8_t*)phead, addr_from);

					if (phead->mode == MODE_STABLE) {
						const auto it = std::find(seq_rec.begin(), seq_rec.end(), phead->seq_all);

						pdebug("seq = %d\n", phead->seq_all);

						if (it != seq_rec.end()) {
							pdebug("packet was already received: skip.\n");
							continue;
						}
						seq_rec.push(phead->seq_all);
					}
					nwrite = tun_ewrite(sock_tun, pdata, phead->length);
					pdebug("packet was sent to tun seq=%d : write %lu bytes\n", phead->seq_all, nwrite);
				}
				catch (std::exception &e) {
					perror("recvfrom / ewrite");
					pdebug("errno = %d\n", errno);
					print_error("%s - the data will be discarded. Continue.\n", e.what());
				}
			}
		}
	}
	this->th_echo->join();
	return true;
}
