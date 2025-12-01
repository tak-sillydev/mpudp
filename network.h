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
#define	abs(n)		(((n) < 0) ? -(n) : (n))

typedef enum _TRANSMIT_MODE {
	MODE_SPEED,
	MODE_STABLE
} TRANSMIT_MODE;

typedef enum _PACKET_TYPE {
	TYPE_GENERAL,
	TYPE_MANAGEMENT
} PACKET_TYPE;

//  0 |-m-|-t-|--- len ---|---- reserved ----|
//  8 |------------- device_id --------------|
// 16 |----- seq_all -----|---- seq_dev -----|

// パケット転送に関わる情報（24バイト）
// 転送される各パケットの前に付加される
typedef struct _TUN_HEADER {
	uint8_t		mode;		// TRANSMIT_MODE を参照
	uint8_t		type;		// PACKET_TYPE を参照
	uint16_t	length;		// データペイロード長
	uint8_t		reserved[4];

	size_t		device_id;	// Ethernetデバイスに振られるID (64bits)

	uint32_t	seq_all;	// 全体シーケンス：同じ番号は同じパケットであることを示す
	uint32_t	seq_dev;	// デバイスシーケンス：同じデバイス上でパケットの連続性を示す
} TUN_HEADER;


// TUN_HEADER は本プログラムのIPレイヤのように動作させる
// ECHO や STAT からは TUN_HEADER を参照しなくてもよいように作れ

//  0 |------------- TUN_HEADER -------------|
// 24 |---- signature ----|------ seq -------|
// 32 |------------- device_id --------------|
// 40 |------------ time_point --------------|

#define	SIGNATURE_ECHO	"Echo"

// 24 + 24 バイト
typedef struct _ECHO_PACKET {
	TUN_HEADER	header;

	char		signature[4];	// かならず TUN_HEADER の直後に配置
	int32_t		seq;

	size_t		device_id;	// このECHOパケットがどのデバイスのものとして送信されたか

	std::chrono::system_clock::time_point	tm_start;

	_ECHO_PACKET() { set_signature(); }

	inline void set_signature() {
		for (size_t i = 0; i < strlen(SIGNATURE_ECHO); i++) { signature[i] = SIGNATURE_ECHO[i];	}
	}
} ECHO_PACKET;


//  0 |------------- TUN_HEADER -------------|
// 24 |---- signature ----|--- rcvd_bytes ---|
// 32 |-- loss_packets ---|-- rcvd_packets --|
// 40 |------------- device_id --------------|

#define	SIGNATURE_STAT	"Stat"

// 24 + 24 バイト
// サーバーからクライアントに送る回線の統計情報
typedef struct _STAT_PACKET {
	TUN_HEADER	header;

	char		signature[4];	// かならず TUN_HEADER の直後に配置
	int32_t		rcvd_bytes;

	uint		loss_packets;
	uint		rcvd_packets;

	size_t		device_id;		// この STAT パケットがどのデバイスのものとして送信されたか

	_STAT_PACKET() { set_signature(); }

	inline void set_signature() {
		for (size_t i = 0; i < strlen(SIGNATURE_STAT); i++) { signature[i] = SIGNATURE_STAT[i];	}
	}
} STAT_PACKET;

typedef struct _SOCKET_PACK {
	int			sock_fd;
	sockaddr_in	remote_addr;
	sockaddr_in	local_addr;
	std::string	eth_name;
	uint32_t	seq_dev;
	size_t		device_id;

	explicit _SOCKET_PACK() : sock_fd(-1), seq_dev(0) {}
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
		seq_dev		= old.seq_dev;
		device_id	= old.device_id;
		old.sock_fd = -1;
	}

	_SOCKET_PACK& operator=(_SOCKET_PACK&& old) noexcept {
		if (this != &old) {
			if (sock_fd != -1) close(sock_fd);

			remote_addr	= old.remote_addr;
			local_addr	= old.local_addr;
			eth_name	= old.eth_name;
			sock_fd		= old.sock_fd;
			seq_dev		= old.seq_dev;
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
