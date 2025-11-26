#include <algorithm>
#include <iomanip>

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

		pdebug("eth[%s]: fd: %d, device_id = %lx, local addr: %s, port: %d\n",
			s.eth_name.c_str(),
			s.sock_fd,
			s.device_id,
			inet_ntoa(s.local_addr.sin_addr),
			ntohs(s.local_addr.sin_port)
		);
	}
	freeaddrinfo(ai);
	return true;
}

void MPUDPTunnelClient::AddDevice(const std::string& device_name) {
	SOCKET_PACK	s;
	STATISTICS	st;

	s.eth_name  = device_name;
	s.device_id = std::hash<std::string>()(device_name);
	
	st.device_id = s.device_id;

	this->socks.emplace_back(std::move(s));
	this->stats.emplace_back(st);

	return;
}

bool MPUDPTunnelClient::_CheckEchoTimeout(std::vector<size_t>& timeout) {
	const auto nw = system_clock::now();
	bool fout = true;

	std::for_each(this->echo_sent.begin(), this->echo_sent.end(),
		[&](TIMEOUT& t) {
			if (duration_cast<milliseconds>(nw - t.sent_time).count() >= PING_TIMEOUT_MSEC) {
				fout = false;
				timeout.emplace_back(t.device_id);
				t = TIMEOUT_DEFAULT;
			}
		}
	);
	return fout;
}

bool MPUDPTunnelClient::_SendEchoPacket() {
	std::vector<size_t>	down_device;

	ECHO_PACKET	*buf = (ECHO_PACKET*)GetHeader();
	ssize_t		nwrite = 0;

	buf->set_signature();
	down_device.reserve(10);

	for (auto& s : this->socks) {
		buf->seq = this->echo_seq;
		buf->device_id = s.device_id;
		buf->tm_start = system_clock::now();
		nwrite = SendTo(s, sizeof(ECHO_PACKET) - sizeof(TUN_HEADER), TYPE_MANAGEMENT);

		if (nwrite < 0) {
			throw std::runtime_error("sendto returned -1");
		}
		if (!this->_CheckEchoTimeout(down_device)) {
			std::for_each(down_device.begin(), down_device.end(),
				[](size_t id){ pdebug("[!] ECHO TIMEOUT : device_id = %lx, device is DOWN?\n", id); }
			);
			//return false;
		}
		TIMEOUT	t = { buf->tm_start, this->echo_seq, s.device_id };
		this->echo_sent.push(t);
		this->echo_seq++;
	}
	return true;
}

bool MPUDPTunnelClient::_ProcessManagementPacket() {
	char	*sign = (char*)this->GetDataPtr();

	if (strncmp(sign, SIGNATURE_ECHO, strlen(SIGNATURE_ECHO)) == 0) {

		// ECHO パケットを受信した

		ECHO_PACKET		*buf = (ECHO_PACKET*)this->GetHeader();
		microseconds	rtt;

		// 受信したパケットの ECHO シーケンスを送信済みシーケンスリストと照合
		// なお、ここではすでに TUN_HEADER レベルでシーケンスが同じものは排除済みであり、
		// MODE_STABLE の重複の影響は受けず、ただ１つのパケットのみが受信される.
		auto t = std::find_if(this->echo_sent.begin(), this->echo_sent.end(),
			[buf](const TIMEOUT& t) { return t.seq == buf->seq; }
		);
		if (t == this->echo_sent.end()) {
			pdebug("ECHO : t is END, seq = %d\n", buf->seq);
			return false;
		}

		// 照合したシーケンス番号に紐付いたデバイスの検索
		// 取得したデバイス情報を統計情報の更新のために用いる
		auto s = std::find_if(this->stats.begin(), this->stats.end(),
			[&t](const STATISTICS& st) { return st.device_id == t->device_id; }
		);
		if (s == this->stats.end()) {
			pdebug("ECHO : s is END, device_id = %lx\n", t->device_id);
			return false;
		}
		rtt = duration_cast<microseconds>(system_clock::now() - buf->tm_start);
		s->rtt_max = max(s->rtt_max, rtt);
		s->rtt_avg = (s->rtt_avg * s->recvd_count + rtt) / (s->recvd_count + 1);
		s->score   = (double)rtt.count() / s->rtt_max.count();
		s->recvd_count++;

		pdebug(
			"ECHO PACKET RECVD :\n"
			"  device_id = %lx, seq = %d\n"
			"  RTT = %d.%d[msec]\n"
			"  RTTmax = %d.%d[msec]\n"
			"  RTTavg = %d.%d[msec]\n"
			"  score = %f\n",
			s->device_id, buf->seq,
			rtt.count() / 1000, rtt.count() % 1000,
			s->rtt_max.count() / 1000, s->rtt_max.count() % 1000,
			s->rtt_avg.count() / 1000, s->rtt_avg.count() % 1000,
			s->score
		);
		*t = TIMEOUT_DEFAULT;
		print_debug("ECHO %lx\n", buf->device_id);
	}
	else if (strncmp(sign, SIGNATURE_STAT, strlen(SIGNATURE_STAT)) == 0) {

		// STAT パケットを受信した

		STAT_PACKET	*buf = (STAT_PACKET*)this->GetHeader();

		auto s = std::find_if(this->stats.begin(), this->stats.end(),
			[buf](const STATISTICS& s) { return s.device_id == buf->device_id; }
		);
		if (s == this->stats.end()) {
			pdebug("stats: s is END, device_id = %lx\n", buf->device_id);
			return false;
		}

		s->rcvd_bytes   = buf->rcvd_bytes;
		s->loss_packets = buf->loss_packets;
		s->rcvd_packets = buf->rcvd_packets;

		pdebug(
			"STATS PACKET RECVD :\n"
			"  device_id = %lx\n"
			"  speed = %d[B/s]\n"
			"  loss_rate = %f[%%]\n",
			buf->device_id,
			buf->rcvd_bytes,
			(buf->rcvd_bytes == 0) ? 0 : (double)buf->loss_packets / buf->rcvd_packets
		);
		print_debug("STAT %lx\n", buf->device_id);
	}
	else {
		pdebug("signature is not valid.\n");
		return false;
	}
	for (const auto& s : this->stats) {
		auto tm = system_clock::to_time_t(system_clock::now());

		print_debug(
			"[%d] ID=%lx, RTT(avg)=%d.%d[ms], SPD=%d[B/s], LPR=%f[%%]\n",
			tm, s.device_id, s.rtt_avg / 1000, s.rtt_avg % 1000, s.rcvd_bytes,
			(s.rcvd_packets == 0) ? 0 : s.loss_packets / (double)s.rcvd_packets
		);
	}
	return true;
}

/*
 * クライアントモード送受ループ
 * クライアントモードモードでは、socks の各要素はそれぞれの eth デバイスに割り当てられたソケット
 * イテレータを走査してデータを受信する
 */
bool MPUDPTunnelClient::MainLoop() {
	fd_set	rfds;
	int		max_fd = -1;
	int		sel;

	auto socks_it = socks.begin();
	auto phead = this->GetHeader();
	auto pdata = this->GetDataPtr();

	uint32_t	nread, nwrite;
	timeval		tv = { 0, 0 };	// 初期値、タイムアウト 10ms

	system_clock::time_point	nw;

	/*
	 * 受信済みのパケット番号を記録する場所
	 * MODE_STABLE で送信されたパケットは全部の経路に同じものを流して冗長化するので、
	 * 受信側で「すでに受信した」パケットは廃棄する必要がある。
	 * このバッファは溢れた場合、古いものから自動的に削除される仕組み。
	 */
	ringbuf<decltype(GetHeader()->seq_all), 64>	seq_rec(-1);

	std::for_each(
		socks.begin(), socks.end(),
		[&max_fd](const SOCKET_PACK& s){ max_fd = max(s.sock_fd, max_fd); }
	);
	max_fd = max(this->sock_tun, max_fd);

	while (true) {

		// 初期化と使用するソケットのシステム側への通知
		FD_ZERO(&rfds);
		FD_SET(this->sock_tun, &rfds);
		for (const auto& s : this->socks) { FD_SET(s.sock_fd, &rfds); }

		// データを受信するまで待機
		sel = select(max_fd + 1, &rfds, NULL, NULL, &tv);

		if (sel < 0) {
			if (errno == EINTR) continue;

			perror("select()");
			print_error("errno = %d\n", errno);
			return false;
		}
		if (sel == 0) {
			// タイムアウト
			// ECHOを送る時間
			this->_SendEchoPacket();

			tv.tv_sec  = 1;
			tv.tv_usec = 0;
			nw = system_clock::now();
			continue;
		}
		else {
			// 何らかのデバイスからパケットを受信
			if (FD_ISSET(this->sock_tun, &rfds)) {
				/* 
				* TUN デバイス側からデータを受信
				* ここに書き込まれるデータは生のIPパケット
				* ETH デバイスを選定してデータを書き込む（ネットワーク側に流す）
				*/
				try {
					pdebug("\n===== TUN DEVICE RECEIVED DATA =====\n");
					// this->data_buf にデータを書き込んでおくと勝手に運んでくれる
					nread = tun_eread(sock_tun, pdata, BUFSIZE);
					pdebug_tunrecv(nread, pdata);

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
						sockaddr_in	addr_from;

						nread = this->RecvFrom(s, &addr_from);

						pdebug_ethrecv(phead->seq_all, nread, (uint8_t*)phead, addr_from);

						if (phead->mode == MODE_STABLE) {
							const auto it = std::find(seq_rec.begin(), seq_rec.end(), phead->seq_all);
							if (it != seq_rec.end()) {
								pdebug("packet was already received: skip.\n");
								continue;
							}
							seq_rec.push(phead->seq_all);

							if (phead->type == TYPE_MANAGEMENT) {
								this->_ProcessManagementPacket();
								continue;
							}
						}
						nwrite = tun_ewrite(sock_tun, pdata, phead->length);
						pdebug("packet was sent to tun seq=%d : write %lx bytes\n", phead->seq_all, nwrite);
					}
					catch (std::exception &e) {
						perror("recvfrom / ewrite");
						pdebug("errno = %d\n", errno);
						print_error("%s - the data will be discarded. Continue.\n", e.what());
					}
				}
			}
			settimer_1sec(tv, nw);
		}
	}
	return true;
}
