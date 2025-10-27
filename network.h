#ifndef __NETWORK_H__
#define	__NETWORK_H__

//#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/if_tun.h>
#include <fcntl.h>
//#include <sys/types.h>
//#include <netinet/in.h>
#include <netdb.h>

#include <unistd.h>
//#include <stdio.h>
#include <string.h>

#include <string>
#include <stdexcept>
#include <vector>
#include <chrono>

#define	max(a, b)	(((a) > (b)) ? (a) : (b))

#define	BUFSIZE		2048

#define	PACKET_TYPE_GENERAL	0
#define	PACKET_TYPE_MANAGE	1

typedef enum _MANAGE_TYPE {
	MANAGE_TYPE_ECHO
} MANAGE_TYPE;

typedef enum _TRANSMIT_MODE {
	MODE_SPEED,
	MODE_STABLE
} TRANSMIT_MODE;

// パケット転送に関わる情報（8バイト）
// 転送される各パケットの前に付加される
typedef struct {
	uint8_t		mode: 7;	// TRANSMIT_MODE を参照
	uint8_t		type: 1;	// 0 = 一般パケット、1 = 管理用パケット
	uint8_t		device_id;	// Ethernetデバイスに振られるID（実際にはソケットのFD）
	uint16_t	length;		// データペイロード長
	uint32_t	seq;		// シーケンス（MODE = SPEED / STABLEで扱いが異なる）
} TUN_HEADER;

using std::chrono::system_clock;

typedef struct {
	TUN_HEADER	tun_header;
	uint8_t		type;
	uint8_t		rsvd[3];
	uint32_t	seq;
	system_clock::time_point	tm_start;
} ECHO_PACKET;


typedef struct _SOCKET_PACK {
	int			sock_fd;
	sockaddr_in	remote_addr;
	sockaddr_in	local_addr;
	std::string	eth_name;

	explicit _SOCKET_PACK() : sock_fd(-1) {}
	~_SOCKET_PACK() {
		if (sock_fd != -1) { close(sock_fd); }
	}

	// デストラクタが呼ばれることによる意図せぬクローズを防ぐため、コピーを禁止
	_SOCKET_PACK(const _SOCKET_PACK&) = delete;
	_SOCKET_PACK& operator=(const _SOCKET_PACK&) = delete;

	// かわりにムーブを強制する
	_SOCKET_PACK(_SOCKET_PACK&& old) noexcept {
		remote_addr	= old.remote_addr;
		local_addr	= old.local_addr;
		eth_name	= old.eth_name;
		sock_fd		= old.sock_fd;
		old.sock_fd = -1;
	}

	_SOCKET_PACK& operator=(_SOCKET_PACK&& old) noexcept {
		if (this != &old) {
			if (sock_fd != -1) close(sock_fd);

			remote_addr	= old.remote_addr;
			local_addr	= old.local_addr;
			eth_name	= old.eth_name;
			sock_fd		= old.sock_fd;
			old.sock_fd = -1;
		}
		return *this;
	}
} SOCKET_PACK;

bool is_same_addr(const sockaddr_in& a, const sockaddr_in& b);
int tun_alloc(const char *device_name);
int tun_eread(int fd, void *buf, int n);
int tun_ewrite(int fd, void *buf, int n);
int tun_readn(int fd, void *buf, int n);

#endif
