
#ifndef __MPUDP_H__
#define	__MPUDP_H__

#include <vector>
#include <string>
#include <memory>
#include <thread>
#include <mutex>
#include <chrono>

#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

#include "print.h"
#include "network.h"

// RAIIを装備したほうが良い気がする
class MPUDPTunnel {
private:
	uint32_t	seq;
	std::unique_ptr<uint8_t>	tun_buf;	// 操作時 MUTEX 必須！
	uint8_t	*data_buf;						// 操作時 MUTEX 必須！

	ssize_t _sendto(const SOCKET_PACK& s, const uint16_t data_len);

protected:
	std::mutex	buf_mtx;
	std::vector<SOCKET_PACK>	socks;
	int	sock_tun;

public:
	explicit MPUDPTunnel(uint32_t szbuf);
	~MPUDPTunnel() { socks.clear(); if (sock_tun != -1) { close(sock_tun); } } 

	bool SetTunDevice(const char* tun_name);	// TUN デバイスの確保（デバイスは事前に要セットアップ）

	ssize_t SendTo(								// for MODE_SPEED
		const SOCKET_PACK& s, const uint16_t data_len, const uint8_t type = PACKET_TYPE_GENERAL
	);
	ssize_t SendToAllDevices(					// for MODE_STABLE
		const uint16_t data_len, const uint8_t type = PACKET_TYPE_GENERAL
	);
	ssize_t RecvFrom(const SOCKET_PACK& s, sockaddr_in *addr_from);

	inline TUN_HEADER* const GetHeader() const { return (TUN_HEADER*)tun_buf.get(); }
	inline uint8_t* const GetDataPtr() const { return data_buf; }
	inline const uint32_t GetSeq() const { return seq; }

	virtual bool Start(const std::string& tun_name, const std::string& addr, const int port) = 0;
	virtual bool MainLoop() = 0;
};

using std::chrono::system_clock;
typedef struct _CONNECTIONS {
	system_clock::time_point	connected_time;
	sockaddr_in		addr;
	uint32_t		device_id;	
} CONNECTIONS;

class MPUDPTunnelServer : public MPUDPTunnel {
private:
	std::vector<CONNECTIONS>	connection_list;
	int sock_recv;

	bool Start(const std::string& tun_name, const std::string& addr, const int port) override;
	void _RefreshConnection(sockaddr_in& addr_from);
	void _process_echo_packet();

public:
	MPUDPTunnelServer(uint32_t szbuf) : MPUDPTunnel(szbuf) {}
	~MPUDPTunnelServer() {}

	ssize_t RecvFrom(sockaddr_in *addr_from);
	bool MainLoop() override;

	inline bool Listen(const std::string& tun_name, const int port) { return Start(tun_name, "", port); }
};


class MPUDPTunnelClient : public MPUDPTunnel {
private:
	bool _GetAddressInfo(const std::string& dst_addr, const int dst_port, addrinfo **result);
	std::unique_ptr<std::thread> _send_echo_thread();
	void _process_echo_packet(const ECHO_PACKET *echo);

	bool Start(const std::string& tun_name, const std::string& addr, const int port) override;

public:
	explicit MPUDPTunnelClient(uint32_t szbuf) : MPUDPTunnel(szbuf) {};
	~MPUDPTunnelClient() {}

	void AddDevice(const std::string& device_name);
	bool MainLoop() override;

	inline bool Connect(const std::string& tun_name, const std::string& addr, const int port) {
		return Start(tun_name, addr, port);
	}
};

#endif
