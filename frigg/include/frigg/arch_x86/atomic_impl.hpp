
namespace frigg {

template<typename T, typename = void>
struct Atomic;

template<>
struct Atomic<int32_t> {
	static bool compareSwap(int32_t *pointer,
			int32_t expect, int32_t overwrite, int32_t &found) {
		bool success;
		asm volatile ( "lock cmpxchgl %3, %4" : "=@ccz" (success), "=a" (found)
				: "1" (expect), "r" (overwrite), "m" (*pointer) : "memory" );
		return success;
	}

	static void fetchInc(int32_t *pointer, int32_t &old_value) {
		asm volatile ( "lock xaddl %0, %1" : "=r" (old_value)
				: "m" (*pointer), "0" (1) : "memory" );
	}
	
	static void fetchDec(int32_t *pointer, int32_t &old_value) {
		asm volatile ( "lock xaddl %0, %1" : "=r" (old_value)
				: "m" (*pointer), "0" (-1) : "memory" );
	}
};

template<>
struct Atomic<uint32_t> {
	static void fetchInc(uint32_t *pointer, uint32_t &old_value) {
		asm volatile ( "lock xaddl %0, %1" : "=r" (old_value)
				: "m" (*pointer), "0" (1) : "memory" );
	}
};

template<>
struct Atomic<int64_t> {
	static void fetchInc(int64_t *pointer, int64_t &old_value) {
		asm volatile ( "lock xaddq %0, %1" : "=r" (old_value)
				: "m" (*pointer), "0" (1) : "memory" );
	}
};

template<typename T>
inline void volatileWrite(T *pointer, T value) {
	*const_cast<volatile T *>(pointer) = value;
}
template<typename T>
inline T volatileRead(T *pointer) {
	return *const_cast<volatile T *>(pointer);
}

inline void pause() {
	asm volatile ( "pause" );
}

template<typename T>
bool compareSwap(T *pointer, T expect, T overwrite, T &found) {
	return Atomic<T>::compareSwap(pointer, expect, overwrite, found);
}

template<typename T>
void fetchInc(T *pointer, T &old_value) {
	Atomic<T>::fetchInc(pointer, old_value);
}

template<typename T>
void fetchDec(T *pointer, T &old_value) {
	Atomic<T>::fetchDec(pointer, old_value);
}

class TicketLock {
public:
	TicketLock()
	: p_nextTicket(0), p_servingTicket(0) { }

	void lock() {
		uint32_t ticket;
		fetchInc<uint32_t>(&p_nextTicket, ticket);

		while(volatileRead<uint32_t>(&p_servingTicket) != ticket) {
			pause();
		}
	}
	
	void unlock() {
		volatileWrite<uint32_t>(&p_servingTicket, p_servingTicket + 1);
	}

private:
	uint32_t p_nextTicket;
	uint32_t p_servingTicket;
};

} // namespace frigg
