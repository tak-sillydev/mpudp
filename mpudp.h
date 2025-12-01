
#ifndef __MPUDP_H__
#define	__MPUDP_H__

#include <vector>
#include <string>
#include <memory>
#include <chrono>

#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

#include "mpudpdef.h"
#include "print.h"
#include "network.h"
#include "ringbuf.h"

namespace chr = std::chrono;

//////////////////////////===============================//////////////////////////
//////////////////////////  MPUDP-Tunnel Base Structures //////////////////////////
//////////////////////////===============================//////////////////////////

// RAIIを装備したほうが良い気がする
class MPUDPTunnel {
private:
	std::unique_ptr<uint8_t>	tun_buf;
	uint8_t		*data_buf;
	uint32_t	seq;

	ssize_t _sendto(SOCKET_PACK& s, uint16_t data_len);

protected:
	std::vector<SOCKET_PACK>	socks;
	int			sock_tun;

	inline void settimer_1sec(timeval& tv, chr::system_clock::time_point base) {
		tv.tv_sec  = 0;
		tv.tv_usec = (1 * 1000 * 1000) - chr::duration_cast<chr::microseconds>(chr::system_clock::now() - base).count();

		if (tv.tv_usec < 0) tv.tv_usec = 0;
		return;
	}
	virtual bool _ProcessManagementPacket() = 0;

public:
	explicit MPUDPTunnel(uint32_t szbuf);
	~MPUDPTunnel();

	bool SetTunDevice(const char* tun_name);	// TUN デバイスの確保（デバイスは事前に要セットアップ）

	ssize_t SendTo(SOCKET_PACK& s, uint16_t data_len, uint8_t type = TYPE_GENERAL);	// for MODE_SPEED
	ssize_t SendToAllDevices(uint16_t data_len, uint8_t type = TYPE_GENERAL);		// for MODE_STABLE
	ssize_t RecvFrom(SOCKET_PACK& s, sockaddr_in *addr_from);

	inline TUN_HEADER* const GetHeader() const { return (TUN_HEADER*)tun_buf.get(); }
	inline uint8_t* const GetDataPtr() const { return data_buf; }
	inline const uint32_t GetSeq() const { return seq; }

	virtual bool MainLoop() = 0;
};





//////////////////////////=================================//////////////////////////
//////////////////////////  MPUDP-Tunnel Server Structures //////////////////////////
//////////////////////////=================================//////////////////////////

// 接続元・統計情報
typedef struct _CONNECTIONS {
	chr::system_clock::time_point	connected_time;
	sockaddr_in	addr;
	size_t		device_id;
	int32_t		rcvd_bytes;
	uint32_t	last_seq;
	uint		loss_packets;
	uint		rcvd_packets;
	ringbuf<int32_t, 256>	loss_buf;

	_CONNECTIONS() :
		connected_time(chr::system_clock::time_point::min()), rcvd_bytes(0),
		last_seq(0), loss_packets(0), rcvd_packets(0), loss_buf(-1) {}

	void reset_stats() {
		rcvd_bytes = 0;
		loss_packets = 0;
		rcvd_packets = 0;
		loss_buf.fill(-1);
	}
} CONNECTIONS;

class MPUDPTunnelServer : public MPUDPTunnel {
private:
	std::vector<CONNECTIONS>	conn_list;
	int		sock_recv;

	bool Start(const std::string& tun_name, const int port);
	bool _SetupSocket(int& sock_fd, int listen_port);
	void _RefreshConnection(sockaddr_in& addr_from, int nrecv);
	bool _ProcessManagementPacket() override;
	bool _SendStatsPacket();

public:
	MPUDPTunnelServer(uint32_t szbuf) : MPUDPTunnel(szbuf), sock_recv(-1) {}
	~MPUDPTunnelServer() { if (sock_recv != -1) { close(sock_recv); } }

	ssize_t RecvFrom(sockaddr_in *addr_from);
	bool MainLoop() override;

	inline bool Listen(const std::string& tun_name, const int port) { return Start(tun_name, port); }
};





//////////////////////////=================================//////////////////////////
//////////////////////////  MPUDP-Tunnel Client Structures //////////////////////////
//////////////////////////=================================//////////////////////////

#define	TIMEOUT_DEFAULT	{ chr::system_clock::time_point::min(), -1, 0 }

// クライアントがECHOパケットを送信した時刻を記録保存する
//  サーバからECHOが返って来たとき、seq と device_id で送ったパケットを照合し、
//  合致したものの sent_time と現在時刻の差分を取っておおよその RTT を算出する
//  サーバは冗長化のため、受け取ったECHOパケットに対する返信を既知の経路全てに対して行う。
//  行き帰りで経路（デバイス）が異なる場合があるため「どのデバイスから送られたか（= device_id）」の情報が必要。
typedef struct _TIMEOUT {
	chr::system_clock::time_point	sent_time;
	int32_t		seq;
	size_t		device_id;

	//_TIMEOUT() : sent_time(system_clock::time_point::min()), seq(-1), device_id(0) {}
} TIMEOUT;

// MPUDPTunnelClient が内部で保持する接続統計情報
//  どのデバイスが、どれだけパケットの交換能力を有するかを計算・保持して回線選択の補助に使用する。
typedef struct _STATISTICS {
	size_t		device_id;
	double		score;

	// ECHO 関連
	uint64_t	recvd_count;
	std::chrono::microseconds	rtt_max;
	std::chrono::microseconds	rtt_avg;
	//std::chrono::microseconds	rtt_latest;

	// STATS 関連
	int32_t		rcvd_bytes;
	uint		loss_packets;
	uint		rcvd_packets;

	_STATISTICS() :
		device_id(0), score(1), recvd_count(0),
		rtt_max(std::chrono::microseconds::min()),
		rtt_avg(std::chrono::microseconds::min()),
		rcvd_bytes(0), loss_packets(0), rcvd_packets(0) {}
	
	double loss_rate() const {
		return (rcvd_packets == 0) ? 0 : (double)loss_packets / (loss_packets + rcvd_packets);
	}
	double CalculateScore(chr::microseconds rtt_worst);
} STATISTICS;

class MPUDPTunnelClient : public MPUDPTunnel {
private:
	std::vector<STATISTICS>	stats;
	ringbuf<TIMEOUT, 64>	echo_sent;
	int32_t			echo_seq;

	bool _GetAddressInfo(const std::string& dst_addr, const int dst_port, addrinfo **result);
	bool _SetupSocket(int& sock_fd, const addrinfo& ai, const std::string& eth_name);
	bool _CheckEchoTimeout(std::vector<size_t>& timeout);
	bool _SendEchoPacket();
	bool _ProcessManagementPacket() override;
	int  _SelectDeviceDynamic();

	bool Start(const std::string& tun_name, const std::string& addr, const int port);

public:
	// dev_selector は必ずメンバイニシャライザで初期化（operator() と混同されてエラーになる）
	explicit MPUDPTunnelClient(uint32_t szbuf) : MPUDPTunnel(szbuf), echo_seq(0) {
		this->echo_sent.fill(TIMEOUT_DEFAULT);
	};
	~MPUDPTunnelClient() {}

	void AddDevice(const std::string& device_name);
	bool MainLoop() override;

	inline bool Connect(const std::string& tun_name, const std::string& addr, const int port) {
		return Start(tun_name, addr, port);
	}
};

#endif
