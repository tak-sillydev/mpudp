/*
	試作品：IPv4 only 2024-09-07
*/
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <fcntl.h>

#include <errno.h>

#include <stdexcept>


#define	MODE_SERVER	0
#define	MODE_CLIENT	1
#define	BUFSIZE		2048

#define	ETH_LENG	1

#define	DEBUG

#define	max(a, b)	(((a) > (b)) ? (a) : (b))

void print_error(const char *format, ...);
void print_debug(const char *format, ...);
int tun_alloc(char *device_name);

enum {
	MODE_SPEED,
	MODE_STABLE
};

// パケット転送に関わる情報（８バイト）
// 転送される各パケットの前に付加される
typedef struct {
	char	mode;
	char	reserved;
	unsigned short	length;
	unsigned int	stable_id;
} TUN_HEADER;


int tun_alloc(char *device_name) {
	struct ifreq	ifr;
	const char		*clone_device = "/dev/net/tun";
	int		fd, err;

	// /dev/net/tun を開く
	if ((fd = open(clone_device, O_RDWR)) < 0) {
		perror("Opening /dev/net/tun");
		print_error("errno = %d\n", errno);
		return fd;
	}
	memset(&ifr, 0, sizeof(ifr));

	// インターフェース リクエスト（ifreq）の初期化
	// TUN デバイス、かつ Ethernetヘッダを削除（TUNはネットワーク層なので使わない）
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

	if (*device_name) {
		strncpy(ifr.ifr_name, device_name, IFNAMSIZ);
	}
	// 確保したいデバイス名を指定して /dev/net/tun に投げるとアクセス権限が取れる…らしい
	// 事実上の socket() 関数
	if ((err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0) {
		perror("ioctl(TUNSETIFF)");
		print_error("errno = %d\n", errno);
		return err;
	}
	strcpy(device_name, ifr.ifr_name);	// これはなに？
	return fd;
}

int eread(int fd, void *buf, int n) {
	int nread;

	if ((nread = read(fd, buf, n)) < 0) {
		perror("Reading from socket");
		print_error("errno = %d\n", errno);
		throw std::runtime_error("nread smaller than zero");
	}
	return nread;
}

int ewrite(int fd, void *buf, int n) {
	int nwrite;

	if ((nwrite = write(fd, buf, n)) < 0) {
		perror("Writing to socket");
		print_error("errno = %d\n", errno);
		throw std::runtime_error("nwrite smaller than zero");
	}
	return nwrite;
}

int readn(int fd, void *buf, int n) {
	char	*b = (char*)buf;
	int		nread, left = n;

	while (left > 0) {
		if ((nread = eread(fd, b, left)) == 0) {
			return 0;
		}
		left -= nread;
		b += nread;
	}
	return n;
}

void print_error(const char *format, ...) {
	va_list	arg;

	va_start(arg, format);
	vfprintf(stderr, format, arg);
	va_end(arg);
}

void print_debug(const char *format, ...) {
#ifdef DEBUG
	va_list	arg;

	va_start(arg, format);
	vfprintf(stderr, format, arg);
	va_end(arg);
#endif
}

int main(int argc, char* argv[]) {
	fd_set	rfds;
	int		max_fds, err;

/*
	int	option;
	while ((option = getopt(argc, argv, "d:t:sc:")) > 0) {
		switch (option) {
		case 'd':
			break;

		case 't':
			break;
		
		case 's':
			break;

		case 'c':
			break;
		}
	}
*/
	const char	*device_list[1] = { "eth0" };	// データを再送信する実NWデバイス
	char	tun_name[64] = "tun_test";		// データを受け取る tun デバイス
	int		mode = MODE_SERVER;			// クライアントモードで動作

	const char	*dst_addr	= "163.44.119.43";
	const int	dst_port	= 45555;

	// エラーチェック
	if (*tun_name == '\0') {
		print_error("tun device name not specified\n");
		exit(1);
	}

	int	sock_tun;

	// TUN デバイスの確保（デバイスは事前に要セットアップ）
	if ((sock_tun = tun_alloc(tun_name)) < 0) {
		print_error("Couldn't connect to tun device - %s\n", tun_name);
		exit(1);
	}
	print_debug("CONNECT OK - %s\n", tun_name);

	sockaddr_in		*addr_eth;	
	int	*sock_eth;
	int	eth_max = 0;
	int	optval = 1;

	unsigned int	szaddr = sizeof(sockaddr_in);
	ssize_t			szrecv, selret;

	char	sock_buf[BUFSIZE];

	// eth 側ソケット準備
	sock_eth = new int[ETH_LENG];
	addr_eth = new sockaddr_in[ETH_LENG];

	if (sock_eth == nullptr) {
		perror("operator new");
		print_error("Couldn't allocate buffer for socket: errno = %d\n", errno);
		exit(1);
	}
	if (addr_eth == nullptr) {
		perror("operator new");
		print_error("Couldn't allocate buffer for address: errno = %d\n", errno);
		exit(1);
	}

	if (mode == MODE_CLIENT) {
		// クライアントモード
		// getaddrinfo() : hints に条件を入れてアドレスやらポートを指定すると
		// いい感じにほかのパラメータを補って res に結果を入れて返す
		char		port_str[16];
		addrinfo	hints, *res;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family		= AF_INET;
		hints.ai_socktype	= SOCK_DGRAM;

		sprintf(port_str, "%hu", dst_port);
		if ((err = getaddrinfo(dst_addr, port_str, &hints, &res)) < 0) {
			perror("getaddrinfo()");
			print_error("err = %d, reason = %s\n", err, gai_strerror(err));
			print_error("errno = %d\n", errno);
			exit(1);
		}

		for (int i = 0; i < ETH_LENG; i++) {
			// 使用する実デバイスにそれぞれソケットをくっつける
			if ((sock_eth[i] = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0) {
				perror("socket()");
				print_error("Couldn't create socket - %s\n", device_list[i]);
				print_error("errno = %d\n", errno);
				exit(1);
			}
			if (setsockopt(sock_eth[i], SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
				perror("setsockopt()");
				print_error("Couldn't set value of SO_REUSEASSR - %s\n", device_list[i]);
				print_error("errno = %d\n", errno);
				exit(1);
			}
			if (setsockopt(sock_eth[i], SOL_SOCKET, SO_BINDTODEVICE, device_list[i], strlen(device_list[i])) < 0) {
				perror("setsockopt()");
				print_error("Couldn't set value of SO_BINDTODEVICE - %s\n", device_list[i]);
				print_error("errno = %d\n", errno);
				exit(1);
			}
			if (connect(sock_eth[i], res->ai_addr, res->ai_addrlen) < 0) {
				perror("connect()");
				print_error("device = %s, errno = %d\n", device_list[i], errno);
				exit(1);
			}
			// eth側ソケットで使用されるポートを固定する（果たして必要なのか）
			addr_eth[i].sin_family		= AF_INET;
			addr_eth[i].sin_addr.s_addr	= htonl(INADDR_ANY);
			addr_eth[i].sin_port		= htons(0);
			bind(sock_eth[i], (sockaddr*)&addr_eth[i], sizeof(sockaddr));
			getsockname(sock_eth[i], (sockaddr *)&addr_eth[i], &szaddr);		// bind() によって使用ポートが割り当てられたので情報を取得

			print_debug("eth[%d]: addr: %s, port: %d\n",
				i,
				inet_ntoa(addr_eth[i].sin_addr),
				ntohs(addr_eth[i].sin_port)
			);
			eth_max = max(sock_eth[i], eth_max);
		}
		freeaddrinfo(res);
	}
	else {
		// サーバーモード
		if ((*sock_eth = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			perror("socket()");
			print_error("Couldn't create socket - %s\n", device_list[0]);
			print_error("errno = %d\n", errno);
			exit(1);
		}
		if (setsockopt(*sock_eth, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_REUSEASSR - %s\n", device_list[0]);
			print_error("errno = %d\n", errno);
		}
		memset(addr_eth, 0, sizeof(*addr_eth));
		addr_eth->sin_family		= AF_INET;
		addr_eth->sin_addr.s_addr	= htonl(INADDR_ANY);
		addr_eth->sin_port			= htons(dst_port);

		if (bind(*sock_eth, (sockaddr*)addr_eth, sizeof(*addr_eth)) < 0) {
			perror("bind()");
			print_error("errno = %d\n", errno);
			exit(1);
		}
		getsockname(*sock_eth, (sockaddr*)addr_eth, &szaddr);
		print_debug("eth: addr: %s, port: %d\n",
			inet_ntoa(addr_eth->sin_addr),
			ntohs(addr_eth->sin_port)
		);
		eth_max = *sock_eth;
	}
	memset(sock_buf, 0, BUFSIZE);
	max_fds = max(sock_tun, eth_max);

	int	nread, nwrite, tun_cnt = 0, eth_cnt = 0;

	while (true) {

		FD_ZERO(&rfds);
		FD_SET(sock_tun, &rfds);
		for (int i = 0; i < ETH_LENG; i++) { FD_SET(sock_eth[i], &rfds); }

		selret = select(max_fds + 1, &rfds, NULL, NULL, NULL);	// データ到着まで待機

		if (selret < 0) {
			if (errno == EINTR) continue;

			perror("select()");
			print_error("errno = %d\n", errno);
			exit(1);
		}
		if (FD_ISSET(sock_tun, &rfds)) {
			// TUN にデータが入った
			// TUN から取れるデータはIPパケットそのもの（IPヘッダが存在する）
			try {
				nread = eread(sock_tun, (void*)sock_buf, BUFSIZE);
				print_debug("from tun seq=%d : read %lu bytes\n", tun_cnt, nread);

				TUN_HEADER	tun_head = { (char)htons(MODE_SPEED), 0, htons((unsigned short)nread), 0 };

				nwrite  = ewrite(sock_eth[tun_cnt % ETH_LENG], &tun_head, sizeof(tun_head));
				nwrite += ewrite(sock_eth[tun_cnt % ETH_LENG], sock_buf, nread);

				print_debug("to eth seq=%d : write %lu bytes\n", tun_cnt, nwrite);
			}
			catch (std::exception &e) {
				print_error("eread() / ewrite(): %s - the data will be discarded. Continue.\n", e.what());
			}
			tun_cnt++;
		}
		for (int i = 0; i < ETH_LENG; i++) {
			if (FD_ISSET(sock_eth[i], &rfds)) {
				// eth[i] にデータが入った
				/* 
				 * パケットサイズがMTUを超える場合、パケットは複数に分割される
				 * この分割、また受信時の再合成の処理はより低レイヤー（ネットワーク層）で行われるので、
				 * UDPのレイヤでは特に考えなくて良い。ただし、パケットが遅れて到着する可能性はあるので、
				 * 送信したバイト数を読み切るまで待機する処理が必要（ここでは readn が行う）
				 */
				TUN_HEADER	tun_head;

				try {
					nread = readn(sock_eth[i], &tun_head, sizeof(TUN_HEADER));
					if (nread == 0) {
						print_error("something went wrong at the other end\n");
						goto end;	// 対向側でなんか異常がおきたっぽい（Ctrl-Cも含まれるとか）
					}

					// パケット分割に備え、対向側が TUN_HEADER に書き込んだデータ長を読み切るまで待機
					nread += readn(sock_eth[i], sock_buf, ntohs(tun_head.length));
					print_debug("from eth seq=%d : read %lu bytes\n", eth_cnt, nread);

					nwrite = ewrite(sock_tun, sock_buf, ntohs(tun_head.length));
					print_debug("to tun seq=%d : write %lu bytes\n", eth_cnt, nwrite);
				}
				catch (std::exception &e) {
					print_error("eread() / ewrite(): %s - the data will be discarded. Continue.\n", e.what());
				}
				eth_cnt++;
			}
		}
	}
end:
	close(sock_tun);
	for (int i = 0; i < ETH_LENG; i++) { close(sock_eth[i]); }
	delete[] addr_eth;
	delete[] sock_eth;

	return 0;
}