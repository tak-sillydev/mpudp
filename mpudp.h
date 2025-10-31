
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

#include "mpudpdef.h"
#include "print.h"
#include "network.h"
#include "ringbuf.h"

// RAIIを装備したほうが良い気がする
class MPUDPTunnel {
private:
	uint32_t	seq;
	std::unique_ptr<uint8_t>	tun_buf;	// 操作時 MUTEX 必須！
	uint8_t	*data_buf;						// 操作時 MUTEX 必須！

	ssize_t _sendto(SOCKET_PACK& s, uint16_t data_len);

protected:
	std::mutex	buf_mtx;
	std::vector<SOCKET_PACK>	socks;
	int	sock_tun;

	std::unique_ptr<std::thread>	th_echo;

public:
	explicit MPUDPTunnel(uint32_t szbuf);
	~MPUDPTunnel();

	bool SetTunDevice(const char* tun_name);	// TUN デバイスの確保（デバイスは事前に要セットアップ）

	ssize_t SendTo(SOCKET_PACK& s, uint16_t data_len);	// for MODE_SPEED
	ssize_t SendToAllDevices(uint16_t data_len);				// for MODE_STABLE
	ssize_t RecvFrom(SOCKET_PACK& s, sockaddr_in *addr_from);

	inline TUN_HEADER* const GetHeader() const { return (TUN_HEADER*)tun_buf.get(); }
	inline uint8_t* const GetDataPtr() const { return data_buf; }
	inline const uint32_t GetSeq() const { return seq; }

	// MainLoop を純粋仮想関数として宣言してしまっているので下２つの関数はここで宣言する意味は特にない、呼ばれないし。
	// 引数は違っていいので同じ名前の関数を実装しておいてね、という意味で残してある。
	// virtual にしてあるので 子クラスで using しても使えない。
	//virtual bool Start();
	//virtual std::unique_ptr<std::thread> _StartEchoThread();

	virtual bool MainLoop() = 0;
};



using std::chrono::system_clock;
typedef struct _CONNECTIONS {
	system_clock::time_point	connected_time;
	sockaddr_in		addr;
	int32_t		device_id;
} CONNECTIONS;

class MPUDPTunnelServer : public MPUDPTunnel {
private:
	std::vector<CONNECTIONS>	connection_list;
	int sock_recv;

	bool Start(const std::string& tun_name, const int port);
	bool _SetupSocket(int& sock_fd, int listen_port);
	void _RefreshConnection(sockaddr_in& addr_from);

	std::unique_ptr<std::thread> _StartEchoThread();

public:
	MPUDPTunnelServer(uint32_t szbuf) : MPUDPTunnel(szbuf) {}
	~MPUDPTunnelServer() {}

	ssize_t RecvFrom(sockaddr_in *addr_from);
	bool MainLoop() override;

	inline bool Listen(const std::string& tun_name, const int port) { return Start(tun_name, port); }
};



typedef struct _CONNECT_STATUS {
	system_clock::time_point	ping_sent_time;
	int64_t	seq;
	//_CONNECT_STATUS() : device_id(-1), seq(-1) {}
} CONNECT_STATUS;

typedef struct _ECHO_SOCKETS {
	int		device_id;
	int		echo_sock;
	double		score;
	uint64_t	recvd_count;
	std::chrono::microseconds	rtt_max;
	std::chrono::microseconds	rtt_avg;
	ringbuf<CONNECT_STATUS,32>	status;
} ECHO_SOCKETS;

class MPUDPTunnelClient : public MPUDPTunnel {
private:
	bool _GetAddressInfo(const std::string& dst_addr, const int dst_port, addrinfo **result);
	bool _SetupSocket(int& sock_fd, const addrinfo& ai, const std::string& eth_name);

	std::unique_ptr<std::thread> _StartEchoThread(const std::string& dst_addr);// dst_addr はコピーの方がよい

	bool Start(const std::string& tun_name, const std::string& addr, const int port);

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
