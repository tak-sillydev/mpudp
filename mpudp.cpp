#include "mpudp.h"

MPUDPTunnel::MPUDPTunnel(uint32_t szbuf) :
	seq(0), tun_buf(new uint8_t[szbuf + sizeof(TUN_HEADER)]), sock_tun(-1) {
	//this->socks.reserve(10);
	this->data_buf = this->tun_buf.get() + sizeof(TUN_HEADER);
}

MPUDPTunnel::~MPUDPTunnel() {
	if (th_echo->joinable()) {
		th_echo->join();
	}
	if (sock_tun != -1) {
		close(sock_tun);
	}
	socks.clear();
} 

bool MPUDPTunnel::SetTunDevice(const char *tun_name) {
	if ((this->sock_tun = tun_alloc(tun_name)) < 0) {
		print_error("Couldn't connect to tun device - %s\n", tun_name);
		return false;
	}
	pdebug("CONNECT OK - %s\n", tun_name);
	return true;
}

ssize_t MPUDPTunnel::_sendto(SOCKET_PACK& s, uint16_t data_len) {
	ssize_t	nwrite = 0;

	try {
		nwrite = sendto(
			s.sock_fd, tun_buf.get(), sizeof(TUN_HEADER) + data_len, 0,
			(sockaddr*)&(s.remote_addr), sizeof(s.remote_addr)
		);
		if (nwrite < 0) { throw std::runtime_error("sendto returned an invalid value"); }
		pdebug(
			"packet was sent to : %s:%d\n",
			inet_ntoa(s.remote_addr.sin_addr), ntohs(s.remote_addr.sin_port)
		);
		s.seq_dev++;
		
	} catch(std::exception& e) {
		perror("sendto");
		print_error("%s : %d\n", e.what(), errno);
		nwrite = -1;
	}
	return nwrite;
}

// data_len はペイロード長
ssize_t MPUDPTunnel::SendTo(SOCKET_PACK& s, uint16_t data_len) {
	TUN_HEADER	*phead = (TUN_HEADER*)tun_buf.get();
	ssize_t		nwrite = 0;

	phead->device_id = s.sock_fd;
	phead->length = data_len;
	phead->seq_all = this->seq;
	phead->seq_dev = s.seq_dev;
	phead->mode = MODE_SPEED;

	nwrite = this->_sendto(s, data_len);
	this->seq++;
	return nwrite;
}

ssize_t MPUDPTunnel::SendToAllDevices(uint16_t data_len) {
	TUN_HEADER	*phead = (TUN_HEADER*)tun_buf.get();
	ssize_t		nwrite = 0;

	phead->length = data_len;
	phead->seq_all = this->seq;
	phead->mode = MODE_STABLE;

	if (socks.size() == 0) { return 0; }

	for (auto& s : socks) {
		phead->device_id = s.sock_fd;
		nwrite = this->_sendto(s, data_len);
	}
	this->seq++;
	return nwrite;
}

ssize_t MPUDPTunnel::RecvFrom(SOCKET_PACK& s, sockaddr_in *addr_from) {
	TUN_HEADER	*phead = (TUN_HEADER*)tun_buf.get();
	sockaddr_in	addr;
	socklen_t	addr_len = sizeof(addr);
	ssize_t		nread = -1;

	nread = recvfrom(
		s.sock_fd, tun_buf.get(), sizeof(TUN_HEADER),
		MSG_PEEK, (sockaddr*)&addr, &addr_len
	);
	if (nread < 0) {
		throw std::runtime_error("recvfrom returned an invalid value");
	}

	// パケット分割に備え、対向側が TUN_HEADER に書き込んだデータ長を読み切るまで待機
	nread = recvfrom(
		s.sock_fd, tun_buf.get(), sizeof(TUN_HEADER) + phead->length,
		MSG_WAITALL, (sockaddr*)&addr, &addr_len
	);
	if (nread < 0) {
		throw std::runtime_error("recvfrom returned an invalid value");
	}
	if (addr_from != nullptr) {
		*addr_from = addr;
	}
	return nread;
}
