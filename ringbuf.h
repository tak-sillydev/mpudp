#include <array>
#include <stdexcept>

// Nに1を指定した場合にどうなるかはわからん
template<class T, std::size_t N>
class ringbuf: public std::array<T, N> {
private:
	std::size_t	_head, _tail;
	std::size_t _count;

public:
	ringbuf() : _head(0), _tail(0), _count(0) {}
	ringbuf(T def) : _head(0), _tail(0), _count(0) {
		this->fill(def);
	} 

	void push(T n) {
		this->at(_head) = n;

		if (_count < N) _count++;

		_head = (_head >= N - 1) ? 0 : _head + 1;
	}

	T pop() {
		if (_count == N) { _tail = _head; }

		auto& ret = this->at(_tail);

		if (_count > 0) _count--;

		if (_tail == _head && _count == 0) { throw std::runtime_error("all data were already poped."); }
		_tail = (_tail >= N - 1) ? 0 : _tail + 1;

		return ret;
	}
};
