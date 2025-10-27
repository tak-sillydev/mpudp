#include "network.h"
#include "print.h"

bool is_same_addr(const sockaddr_in& a, const sockaddr_in& b) {
	return a.sin_addr.s_addr == b.sin_addr.s_addr &&
			a.sin_port == b.sin_port &&
			a.sin_family == b.sin_family;
}

int tun_alloc(const char *device_name) {
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
	//strcpy(device_name, ifr.ifr_name);	// これはなに？
	return fd;
}

int tun_eread(int fd, void *buf, int n) {
	int nread;

	if ((nread = read(fd, buf, n)) <= 0) {
		perror("Reading from socket");
		print_error("errno = %d\n", errno);
		throw std::runtime_error("nread returned below or equal to zero");
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
