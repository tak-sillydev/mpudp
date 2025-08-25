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
#include <list>

using namespace std;


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

typedef struct {
	int			sock_fd;
	sockaddr_in	remote_addr;
	sockaddr_in	local_addr;
} SOCKET_PACK;


unsigned long hash(void* buf, int size) {
	unsigned long	h = 5361;
	for (int i = 0; i < size; i++) {
		h = ((h << 5) + h + ((char*)buf)[i]);
	}
	return h;
}

bool is_same_addr(const sockaddr_in& a, const sockaddr_in& b) {
	return	a.sin_addr.s_addr == b.sin_addr.s_addr &&
			a.sin_port == b.sin_port &&
			a.sin_family == b.sin_family;
}

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

int tun_eread(int fd, void *buf, int n) {
	int nread;

	if ((nread = read(fd, buf, n)) < 0) {
		perror("Reading from socket");
		print_error("errno = %d\n", errno);
		throw std::runtime_error("nread smaller than zero");
	}
	return nread;
}

int tun_ewrite(int fd, void *buf, int n) {
	int nwrite;

	if ((nwrite = write(fd, buf, n)) < 0) {
		perror("Writing to socket");
		print_error("errno = %d\n", errno);
		throw std::runtime_error("nwrite smaller than zero");
	}
	return nwrite;
}

int tun_readn(int fd, void *buf, int n) {
	char	*b = (char*)buf;
	int		nread, left = n;

	while (left > 0) {
		if ((nread = tun_eread(fd, b, left)) == 0) {
			return 0;
		}
		left -= nread;
		b += nread;
	}
	return n;
}

int eth_recvn(int fd, void* buf, int n, int flags, sockaddr_in* addr) {
	char	*b = (char*)buf;
	int		nread, left = n;
	socklen_t	szaddr = sizeof(*addr);

	while (left > 0) {
		if ((nread = recvfrom(fd, b, left, flags, (sockaddr*)addr, &szaddr)) <= 0) {
			perror("Reading from UDP packet");
			print_error("errno = %d\n", errno);
			throw std::runtime_error("nread below or equal to zero");
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


	int	option;
	int	mode = MODE_CLIENT;

	//while ((option = getopt(argc, argv, "d:t:sc:")) > 0) {
	while ((option = getopt(argc, argv, "s")) > 0) {
		switch (option) {
		case 'd':
			break;

		case 't':
			break;
		
		case 's':
			mode = MODE_SERVER;
			break;

		case 'c':
			break;
		}
	}

	const char	*device_list[1] = { "eth0" };	// データを再送信する実NWデバイス
	char	tun_name[64] = "tun_test";		// データを受け取る tun デバイス

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

	/* 
	 * *socks :
	 * クライアントモードの時は使用するNWデバイスに対応したソケット情報を、
	 * サーバモードの時はそれぞれの接続元に対応したソケット情報を格納する
	 */
	std::list<SOCKET_PACK>	socks;
	int	eth_max = 0;
	int	optval = 1;

	unsigned int	szaddr = sizeof(sockaddr_in);
	ssize_t			selret;

	char	sock_buf[sizeof(TUN_HEADER) + BUFSIZE];

	TUN_HEADER	*pbuf_head = (TUN_HEADER*)sock_buf;
	char		*pbuf_data = sock_buf + sizeof(TUN_HEADER);

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
			SOCKET_PACK	s;

			// 使用する実デバイスにそれぞれソケットをくっつける
			// ソケットの作成とオプションの設定
			if ((s.sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0) {
				perror("socket()");
				print_error("Couldn't create socket - %s\n", device_list[i]);
				print_error("errno = %d\n", errno);
				exit(1);
			}
			if (setsockopt(s.sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
				perror("setsockopt()");
				print_error("Couldn't set value of SO_REUSEASSR - %s\n", device_list[i]);
				print_error("errno = %d\n", errno);
				exit(1);
			}
			if (setsockopt(s.sock_fd, SOL_SOCKET, SO_BINDTODEVICE, device_list[i], strlen(device_list[i])) < 0) {
				perror("setsockopt()");
				print_error("Couldn't set value of SO_BINDTODEVICE - %s\n", device_list[i]);
				print_error("errno = %d\n", errno);
				exit(1);
			}

			// 接続先の設定
			if (connect(s.sock_fd, res->ai_addr, res->ai_addrlen) < 0) {
				perror("connect()");
				print_error("device = %s, errno = %d\n", device_list[i], errno);
				exit(1);
			}
			s.remote_addr = *(sockaddr_in *)res->ai_addr;

			// ローカル側で使用するポートの割当
			s.local_addr.sin_family			= AF_INET;
			s.local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
			s.local_addr.sin_port			= htons(0);
			bind(s.sock_fd, (sockaddr*)&(s.local_addr), sizeof(sockaddr));
			getsockname(s.sock_fd, (sockaddr*)&(s.local_addr), &szaddr);	// bind() によって使用ポートが割り当てられたので情報を取得

			print_debug("eth[%d]: local addr: %s, port: %d\n",
				i,
				inet_ntoa(s.local_addr.sin_addr),
				ntohs(s.local_addr.sin_port)
			);
			eth_max = max(s.sock_fd, eth_max);
			socks.emplace_back(s);
		}
		freeaddrinfo(res);
	}
	else {
		// サーバーモード
		// サーバーモードでは、リストの先頭が接続待ち受けソケットになる
		SOCKET_PACK	s;

		// ソケットの作成とオプションの設定
		if ((s.sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
			perror("socket()");
			print_error("Couldn't create socket - %s\n", device_list[0]);
			print_error("errno = %d\n", errno);
			exit(1);
		}
		if (setsockopt(s.sock_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
			perror("setsockopt()");
			print_error("Couldn't set value of SO_REUSEASSR - %s\n", device_list[0]);
			print_error("errno = %d\n", errno);
		}
		memset(&s.local_addr, 0, sizeof(s.local_addr));
		s.local_addr.sin_family			= AF_INET;
		s.local_addr.sin_addr.s_addr	= htonl(INADDR_ANY);
		s.local_addr.sin_port			= htons(dst_port);

		if (bind(s.sock_fd, (sockaddr*)&s.local_addr, sizeof(s.local_addr)) < 0) {
			perror("bind()");
			print_error("errno = %d\n", errno);
			exit(1);
		}
		getsockname(s.sock_fd, (sockaddr*)&s.local_addr, &szaddr);
		print_debug("eth: addr: %s, port: %d\n",
			inet_ntoa(s.local_addr.sin_addr),
			ntohs(s.local_addr.sin_port)
		);
		socks.emplace_front(s);
		eth_max = s.sock_fd;
	}
	memset(sock_buf, 0, sizeof(TUN_HEADER) + BUFSIZE);
	max_fds = max(sock_tun, eth_max);

	int		nread, nwrite, tun_cnt = 0, eth_cnt = 0;
	auto 	socks_it = socks.cbegin();

	while (true) {

		// サーバーモードの時、sock_fd は全部同じ値なので気にしなくて良い
		FD_ZERO(&rfds);
		FD_SET(sock_tun, &rfds);
		for (auto& s : socks) { FD_SET(s.sock_fd, &rfds); }
		
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
				nread = tun_eread(sock_tun, (void*)pbuf_data, BUFSIZE);
				print_debug("from tun seq=%d : read %lu bytes\n", tun_cnt, nread);
				print_debug("IP header says: packet length = %lu\n", ntohs(((unsigned short*)pbuf_data)[1]));
				print_debug("\tpbuf_head: %02X %02X %02X %02X %02X %02X %02X %02X\n",
					((unsigned char*)pbuf_head)[0], ((unsigned char*)pbuf_head)[1], ((unsigned char*)pbuf_head)[2], ((unsigned char*)pbuf_head)[3],
					((unsigned char*)pbuf_head)[4], ((unsigned char*)pbuf_head)[5], ((unsigned char*)pbuf_head)[6], ((unsigned char*)pbuf_head)[7]
				);
				print_debug("pbuf_data: %02X %02X %02X %02X %02X %02X %02X %02X\n",
					(unsigned char)pbuf_data[0], (unsigned char)pbuf_data[1], (unsigned char)pbuf_data[2], (unsigned char)pbuf_data[3],
					(unsigned char)pbuf_data[4], (unsigned char)pbuf_data[5], (unsigned char)pbuf_data[6], (unsigned char)pbuf_data[7]
				);

				pbuf_head->mode = (char)MODE_SPEED;
				pbuf_head->reserved = 0;
				pbuf_head->length = (unsigned short)nread;	// データ ペイロード長
				pbuf_head->stable_id = 0;

				nread += sizeof(TUN_HEADER);

				if (mode == MODE_CLIENT) {
					// クライアントモード
					// 現状、ラウンドロビン送信
					nwrite = sendto(
						socks_it->sock_fd, sock_buf, nread, 0,
						(sockaddr*)&socks_it->remote_addr, sizeof(socks_it->remote_addr)
					);

					socks_it++;
					if (socks_it == socks.cend()) { socks_it = socks.cbegin(); }
				}
				else {
					/* 
					 * サーバーモード、socks の最初の要素以外にはデータの送信元を表すアドレスのみ格納
					 * sock_fd には先頭の要素と同じものを格納する（bind は行わない）
					 * 最初の要素は待ち受けに使われているソケット
					 * MODE_STABLE より、サーバからはすべての経路に対して同じパケットを送って冗長化
					 */
					print_debug("server to eth, socks len = %d\n", socks.size());
					pbuf_head->mode = (char)MODE_STABLE;

					for (auto it = std::next(socks.cbegin()); it != socks.cend(); it++) {
						nwrite = sendto(
							it->sock_fd, sock_buf, nread, 0,
							(sockaddr*)&it->remote_addr, sizeof(it->remote_addr)
						);
						print_debug(
							"to eth addr: %s, port: %d\n",
							inet_ntoa(it->remote_addr.sin_addr),
							ntohs(it->remote_addr.sin_port)
						);
					}
				}
				print_debug("to eth seq=%d : write %lu bytes\n", tun_cnt, nwrite);
			}
			catch (std::exception &e) {
				print_error("eread() / ewrite(): %s - the data will be discarded. Continue.\n", e.what());
			}
			tun_cnt++;
		}
		if (mode == MODE_CLIENT) {
			/*
			 * クライアントモード
			 * クライアントモードモードでは、socks の各要素はそれぞれの eth デバイスに割り当てられたソケット
			 * イテレータを走査してデータを受信する
			 */
			for (auto it = socks.cbegin(); it != socks.cend(); it++) {
				if (FD_ISSET(it->sock_fd, &rfds)) {
					// eth[i] にデータが入った
					/* 
					 * パケットサイズがMTUを超える場合、パケットは複数に分割される
					 * この分割、また受信時の再合成の処理はより低いレイヤー（ネットワーク層）で行われるので、
					 * UDPのレイヤでは特に考えなくて良い。ただし、パケットが遅れて到着する可能性はあるので、
					 * 送信したバイト数を読み切るまで待機する処理が必要（ここでは readn が行う）
					 */
					sockaddr_in	addr_from;
					socklen_t	from_len;

					try {
						nread = recvfrom(
							it->sock_fd, sock_buf, sizeof(TUN_HEADER),
							MSG_WAITALL | MSG_PEEK, (sockaddr*)&addr_from, &from_len
						);
						print_debug("from eth seq=%d : read %lu bytes\n", eth_cnt, nread);
						// パケット分割に備え、対向側が TUN_HEADER に書き込んだデータ長を読み切るまで待機
						nread += recvfrom(
							it->sock_fd, sock_buf, sizeof(TUN_HEADER) + pbuf_head->length,
							MSG_WAITALL, (sockaddr*)&addr_from, &from_len
						);
						print_debug("from eth seq=%d : read %lu bytes\n", eth_cnt, nread);
						print_debug("IP header says: packet length = %lu\n", ntohs(((unsigned short*)pbuf_data)[1]));
						print_debug("addr: %s, port: %d\n",	inet_ntoa(addr_from.sin_addr), ntohs(addr_from.sin_port));

						nwrite = tun_ewrite(sock_tun, pbuf_data, pbuf_head->length);
						print_debug("to tun seq=%d : write %lu bytes\n", eth_cnt, nwrite);
					}
					catch (std::exception &e) {
						print_error("eread() / ewrite(): %s - the data will be discarded. Continue.\n", e.what());
					}
					eth_cnt++;
				}
			}
		}
		else {
			/*
			 * サーバーモード
			 * サーバーモードでは、監視対象はリストの先頭のソケットのみ
			 * あとのソケットは使用されず、経路情報だけを参照する
			 */
			const auto&	it = socks.cbegin();

			if (FD_ISSET(it->sock_fd, &rfds)) {
				sockaddr_in	addr_from;
				socklen_t	from_len;

				try {
					nread = recvfrom(
						it->sock_fd, sock_buf, sizeof(TUN_HEADER),
						MSG_WAITALL | MSG_PEEK, (sockaddr*)&addr_from, &from_len
					);
					print_debug("\tfrom eth seq=%d : read %lu bytes\n", eth_cnt, nread);
					// パケット分割に備え、対向側が TUN_HEADER に書き込んだデータ長を読み切るまで待機
					nread += recvfrom(
						it->sock_fd, sock_buf, sizeof(TUN_HEADER) + pbuf_head->length,
						MSG_WAITALL, (sockaddr*)&addr_from, &from_len
					);
					print_debug("---- SOCKET RECEIVED ----\n");
					print_debug("\tfrom eth seq=%d : read %lu bytes\n", eth_cnt, nread);
					print_debug("\ttun_head.length = %lu\n", pbuf_head->length);
					print_debug("\tIP header says: packet length = %lu\n", ntohs(((unsigned short*)pbuf_data)[1]));
					print_debug("\tpbuf_head: %02X %02X %02X %02X %02X %02X %02X %02X\n",
						((unsigned char*)pbuf_head)[0], ((unsigned char*)pbuf_head)[1], ((unsigned char*)pbuf_head)[2], ((unsigned char*)pbuf_head)[3],
						((unsigned char*)pbuf_head)[4], ((unsigned char*)pbuf_head)[5], ((unsigned char*)pbuf_head)[6], ((unsigned char*)pbuf_head)[7]
					);
					print_debug("\tpbuf_data: %02X %02X %02X %02X %02X %02X %02X %02X\n",
						(unsigned char)pbuf_data[0], (unsigned char)pbuf_data[1], (unsigned char)pbuf_data[2], (unsigned char)pbuf_data[3],
						(unsigned char)pbuf_data[4], (unsigned char)pbuf_data[5], (unsigned char)pbuf_data[6], (unsigned char)pbuf_data[7]
					);
					print_debug("\taddr: %s, port: %d\n", inet_ntoa(addr_from.sin_addr), ntohs(addr_from.sin_port));

					nwrite = tun_ewrite(sock_tun, pbuf_data, pbuf_head->length);
					print_debug("to tun seq=%d : write %lu bytes\n", eth_cnt, nwrite);
				}
				catch (std::exception &e) {
					print_error("eread() / ewrite(): %s - the data will be discarded. Continue.\n", e.what());
				}
				eth_cnt++;

				// 今までにない経路からの通信なら返信リストに登録
				for (auto it = std::next(socks.cbegin());; it++) {
					if (it == socks.cend()) {
						SOCKET_PACK	s;

						s.sock_fd = socks.begin()->sock_fd;
						s.local_addr  = socks.begin()->local_addr;
						s.remote_addr = addr_from;
						socks.emplace_back(s);
						break;
					}
					if (is_same_addr(addr_from, it->remote_addr)) { break; }
				}
			}
		}
	}
end:
	close(sock_tun);
	socks.clear();

	return 0;
}