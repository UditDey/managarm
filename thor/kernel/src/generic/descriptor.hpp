
namespace thor {

// --------------------------------------------------------
// Memory related descriptors
// --------------------------------------------------------

struct MemoryAccessDescriptor {
	MemoryAccessDescriptor(frigg::SharedPtr<Memory> memory)
	: memory(frigg::move(memory)) { }

	frigg::SharedPtr<Memory> memory;
};

struct AddressSpaceDescriptor {
	AddressSpaceDescriptor(frigg::SharedPtr<AddressSpace> space)
	: space(frigg::move(space)) { }

	frigg::SharedPtr<AddressSpace> space;
};

// --------------------------------------------------------
// Threading related descriptors
// --------------------------------------------------------

struct UniverseDescriptor {
	UniverseDescriptor(frigg::SharedPtr<Universe> universe)
	: universe(frigg::move(universe)) { }

	frigg::SharedPtr<Universe> universe;
};

struct ThreadDescriptor {
	ThreadDescriptor(frigg::SharedPtr<Thread, ThreadRunControl> thread)
	: thread(frigg::move(thread)) { }
	
	frigg::SharedPtr<Thread, ThreadRunControl> thread;
};

// --------------------------------------------------------
// Event related descriptors
// --------------------------------------------------------

struct EventHubDescriptor {
	EventHubDescriptor() = default;

	explicit EventHubDescriptor(frigg::SharedPtr<EventHub> event_hub)
	: eventHub(frigg::move(event_hub)) { }

	frigg::SharedPtr<EventHub> eventHub;
};

// --------------------------------------------------------
// IPC related descriptors
// --------------------------------------------------------

struct StreamControl;
struct Stream;

struct AdoptLane { };
static constexpr AdoptLane adoptLane;

// TODO: implement SharedLaneHandle + UnsafeLaneHandle?
struct LaneHandle {
	friend void swap(LaneHandle &a, LaneHandle &b) {
		using frigg::swap;
		swap(a._stream, b._stream);
		swap(a._lane, b._lane);
	}

	LaneHandle() = default;

	explicit LaneHandle(AdoptLane, frigg::UnsafePtr<Stream> stream, int lane)
	: _stream(stream), _lane(lane) { }

	LaneHandle(const LaneHandle &other);

	LaneHandle(LaneHandle &&other)
	: LaneHandle() {
		swap(*this, other);
	}

	~LaneHandle();

	LaneHandle &operator= (LaneHandle other) {
		swap(*this, other);
		return *this;
	}

	frigg::UnsafePtr<Stream> getStream() {
		return _stream;
	}

	int getLane() {
		return _lane;
	}

private:
	frigg::UnsafePtr<Stream> _stream;
	int _lane;
};

struct LaneDescriptor {
	LaneDescriptor() = default;

	explicit LaneDescriptor(LaneHandle handle)
	: handle(frigg::move(handle)) { }

	LaneHandle handle;
};

struct RingDescriptor {
	RingDescriptor(frigg::SharedPtr<RingBuffer> ring_buffer)
	: ringBuffer(frigg::move(ring_buffer)) { }
	
	frigg::SharedPtr<RingBuffer> ringBuffer;
};

struct EndpointDescriptor {
	EndpointDescriptor(frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint)
	: endpoint(frigg::move(endpoint)) { }
	
	frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint;
};

// --------------------------------------------------------
// IO related descriptors
// --------------------------------------------------------

struct IrqDescriptor {
	IrqDescriptor(frigg::SharedPtr<IrqLine> irq_line)
	: irqLine(frigg::move(irq_line)) { }
	
	frigg::SharedPtr<IrqLine> irqLine;
};

struct IoDescriptor {
	IoDescriptor(frigg::SharedPtr<IoSpace> io_space)
	: ioSpace(frigg::move(io_space)) { }
	
	frigg::SharedPtr<IoSpace> ioSpace;
};

// --------------------------------------------------------
// AnyDescriptor
// --------------------------------------------------------

typedef frigg::Variant<
	MemoryAccessDescriptor,
	AddressSpaceDescriptor,
	UniverseDescriptor,
	ThreadDescriptor,
	EventHubDescriptor,
	LaneDescriptor,
	RingDescriptor,
	EndpointDescriptor,
	IrqDescriptor,
	IoDescriptor
> AnyDescriptor;

} // namespace thor
