#include <algorithm>
#include <random>

#include <sys/types.h>
#include <netdb.h>

#include "ringbuf.h"
#include "print.h"
#include "mpudp.h"

constexpr int _balance = BALANCE_ROUNDROBIN;

double _STATISTICS::CalculateScore(chr::microseconds rtt_worst) {
	double	ping_score, lpr_score;
	double	lpr_target = 0.01f;		// パケロス率 1%を目標に設定、現状とのズレが大きいほどスコアを小さく

	// ping_score : UDPing 値が小さいほどスコアが高い
	// microseconds 同士で割り算すると結果が int64_t になって正規化に失敗する
	// rtt_avg（平均値）でやる意味はあるのだろうか
	ping_score = (double)this->rtt_avg.count() / rtt_worst.count();	// 0-1の範囲で RTT を正規化（1に近づくほど悪い）
	ping_score = abs(ping_score - 1);								// -1 を最良値、0 を最悪値にして絶対値化

	//lpr_score = 1 - this->loss_rate();
	lpr_score = lpr_target - this->loss_rate();

	this->score += (0.1 * ping_score + lpr_score) * this->score;

	if (this->score > 1) { this->score = 1.0f; }
	print_debug("SCORE : id=%lx, ps=%f, ls=%f, ttl=%f\n", this->device_id, ping_score, lpr_score, this->score);

	return this->score;
}

int MPUDPTunnelClient::_SelectDeviceDynamic() {
	static std::random_device	rd;
	static std::mt19937			sel(rd());
	static std::vector<double>	prob(this->stats.size());

	// 現状 AddDevice() で socks と stats は同時に要素の追加を行っているので
	// ソートについては気にする必要がない…が
	for (size_t i = 0; i < stats.size(); i++) { prob[i] = stats[i].score; }
	std::discrete_distribution<>	dist(prob.begin(), prob.end());

	return dist(sel);
}

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
	const auto nw = chr::system_clock::now();
	bool fout = true;

	std::for_each(this->echo_sent.begin(), this->echo_sent.end(),
		[&](TIMEOUT& t) {
			if (chr::duration_cast<chr::milliseconds>(nw - t.sent_time).count() >= PING_TIMEOUT_MSEC) {
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
		buf->tm_start = chr::system_clock::now();
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
		chr::microseconds	rtt;
		chr::microseconds	rtt_worst = chr::microseconds::min();

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
		rtt = chr::duration_cast<chr::microseconds>(chr::system_clock::now() - buf->tm_start);
		s->rtt_max = max(s->rtt_max, rtt);
		s->rtt_avg = (s->rtt_avg * s->recvd_count + rtt) / (s->recvd_count + 1);
		s->recvd_count++;

		// 全回線の UDPing 最大値（最も遅い）を取得
		std::for_each(stats.begin(), stats.end(),
			[&rtt_worst](const STATISTICS& st) { rtt_worst = max(rtt_worst, st.rtt_max); }
		);
		s->CalculateScore(rtt_worst);

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
		chr::microseconds	rtt_worst = chr::microseconds::min();

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

		// 全回線の UDPing 最大値（最も遅い）を取得
		std::for_each(stats.begin(), stats.end(),
			[&rtt_worst](const STATISTICS& st) { rtt_worst = max(rtt_worst, st.rtt_max); }
		);
		s->CalculateScore(rtt_worst);

		pdebug(
			"STATS PACKET RECVD :\n"
			"  device_id = %lx\n"
			"  speed = %d[B/s]\n"
			"  loss_rate = %f[%%]\n",
			buf->device_id,
			buf->rcvd_bytes,
			(buf->rcvd_bytes == 0) ? 0 : (double)buf->loss_packets / (buf->rcvd_packets + buf->loss_packets) * 100
		);
		print_debug("STAT %lx\n", buf->device_id);
	}
	else {
		pdebug("signature is not valid.\n");
		return false;
	}
	for (const auto& s : this->stats) {
		auto tm = chr::system_clock::to_time_t(chr::system_clock::now());

		print_debug(
			"[%d] ID=%lx, RTT(avg)=%d.%d[ms], SPD=%d[B/s], LPR=%f[%%], score=%f\n",
			tm, s.device_id, s.rtt_avg / 1000, s.rtt_avg % 1000, s.rcvd_bytes,
			s.loss_rate() * 100, s.score
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

	chr::system_clock::time_point	nw;

	std::vector<int>	selected_devices;

	selected_devices.resize(socks.size(), 0);

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
			nw = chr::system_clock::now();
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

					if (_balance == BALANCE_ROUNDROBIN) {
						// ラウンドロビンでデータを送る
						// ここにパケットを効率よく分散する機構を組み込む
						nwrite = this->SendTo(*socks_it, nread);
						pdebug("packet was sent to eth device = %s: %lu bytes\n", socks_it->eth_name.c_str(), nwrite);

						socks_it++;
						if (socks_it == socks.cend()) { socks_it = socks.begin(); }
					}
					else if (_balance == BALANCE_DYNAMICSEL) {
						// ダイナミック・ロードバランシング
						// 回線スコアを見て、負荷の少なそうなところにパケットを動的に割り振る
						int index = this->_SelectDeviceDynamic();

						nwrite = this->SendTo(socks[index], nread);
						selected_devices[index]++;
						pdebug("packet was sent to eth device = %s: %lu bytes\n", socks[index].eth_name.c_str(), nwrite);
					}
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
