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

#define	max(a, b)	(((a) > (b)) ? (a) : (b))

enum {
	MODE_SPEED,
	MODE_STABLE
};

// パケット転送に関わる情報（8バイト）
// 転送される各パケットの前に付加される
typedef struct {
	char	mode;
	unsigned char	device_id;	// Ethernetデバイスに振られるID（実際にはソケットのFD）
	unsigned short	length;		// データペイロード長
	unsigned int	seq;		// シーケンス（MODE = SPEED / STABLEで扱いが異なる）
} TUN_HEADER;

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

class SocketManager {
private:
	std::vector<SOCKET_PACK>	socks;

public:
	virtual bool setup(int& max_fd, const char* addr, const int port);
};

bool is_same_addr(const sockaddr_in& a, const sockaddr_in& b);
int tun_alloc(char *device_name);
int tun_eread(int fd, void *buf, int n);
int tun_ewrite(int fd, void *buf, int n);
int tun_readn(int fd, void *buf, int n);

#endif
