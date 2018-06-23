
#include <string.h>

#include <frg/container_of.hpp>
#include "kernel.hpp"
#include "ipc-queue.hpp"
#include "irq.hpp"
#include "../arch/x86/debug.hpp"

using namespace thor;

void readUserMemory(void *kern_ptr, const void *user_ptr, size_t size) {
	enableUserAccess();
	memcpy(kern_ptr, user_ptr, size);
	disableUserAccess();
}

void writeUserMemory(void *user_ptr, const void *kern_ptr, size_t size) {
	enableUserAccess();
	memcpy(user_ptr, kern_ptr, size);
	disableUserAccess();
}

template<typename T>
T readUserObject(const T *pointer) {
	T object;
	readUserMemory(&object, pointer, sizeof(T));
	return object;
}

template<typename T>
void writeUserObject(T *pointer, T object) {
	writeUserMemory(pointer, &object, sizeof(T));
}

template<typename T>
void readUserArray(const T *pointer, T *array, size_t count) {
	for(size_t i = 0; i < count; ++i)
		array[i] = readUserObject(pointer + i);
}

template<typename T>
void writeUserArray(T *pointer, const T *array, size_t count) {
	for(size_t i = 0; i < count; ++i)
		writeUserObject(pointer + i, array[i]);
}

// TODO: one translate function per error source?
HelError translateError(Error error) {
	switch(error) {
	case kErrSuccess: return kHelErrNone;
	case kErrThreadExited: return kHelErrThreadTerminated;
	case kErrLaneShutdown: return kHelErrLaneShutdown;
	case kErrEndOfLane: return kHelErrEndOfLane;
//		case kErrBufferTooSmall: return kHelErrBufferTooSmall;
	case kErrFault: return kHelErrFault;
	default:
		assert(!"Unexpected error");
		__builtin_unreachable();
	}
}

template<typename P>
struct PostEvent {
public:
	struct Wrapper : QueueNode {
		template<typename... Args>
		Wrapper(uintptr_t context, Args &&... args)
		: _writer{frigg::forward<Args>(args)...} {
			setupContext(context);
			setupSource(&_writer.source);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

	private:
		P _writer;
	};

	PostEvent(frigg::SharedPtr<UserQueue> queue, uintptr_t context)
	: _queue(frigg::move(queue)), _context(context) {
		_thread = getCurrentThread();
	}
	
	template<typename... Args>
	void operator() (Args &&... args) {
		auto wrapper = frigg::construct<Wrapper>(*kernelAlloc,
				_context, frigg::forward<Args>(args)...);
		wrapper->setup(_thread->associatedWorkQueue());
		_queue->submit(wrapper);
	}

private:
	frigg::UnsafePtr<Thread> _thread;

	frigg::SharedPtr<UserQueue> _queue;
	uintptr_t _context;
};

struct LockMemoryWriter {
	LockMemoryWriter(Error error)
	: source{&_result, sizeof(HelSimpleResult), nullptr},
			_result{translateError(error), 0} { }

	QueueSource source;

private:
	HelSimpleResult _result;
};

struct ObserveThreadWriter {
	ObserveThreadWriter(Error error, uint64_t sequence, Interrupt interrupt)
	: source{&_result, sizeof(HelObserveResult), nullptr},
			_result{translateError(error), 0, sequence} {
		if(interrupt == kIntrNull) {
			_result.observation = kHelObserveNull;
		}else if(interrupt == kIntrRequested) {
			_result.observation = kHelObserveInterrupt;
		}else if(interrupt == kIntrPanic) {
			_result.observation = kHelObservePanic;
		}else if(interrupt == kIntrBreakpoint) {
			_result.observation = kHelObserveBreakpoint;
		}else if(interrupt == kIntrGeneralFault) {
			_result.observation = kHelObserveGeneralFault;
		}else if(interrupt == kIntrPageFault) {
			_result.observation = kHelObservePageFault;
		}else if(interrupt >= kIntrSuperCall) {
			_result.observation = kHelObserveSuperCall + (interrupt - kIntrSuperCall);
		}else{
			frigg::panicLogger() << "Unexpected interrupt" << frigg::endLog;
			__builtin_unreachable();
		}
	}

	QueueSource source;

private:
	HelObserveResult _result;
};

HelError helLog(const char *string, size_t length) {
	size_t offset = 0;
	while(offset < length) {
		auto chunk = frigg::min(length - offset, size_t{100});

		char buffer[100];
		readUserArray(string + offset, buffer, chunk);
		{
			auto p = frigg::infoLogger();
			for(size_t i = 0; i < chunk; i++)
				p.print(buffer[i]);
		}
		offset += chunk;
	}

	return kHelErrNone;
}


HelError helCreateUniverse(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto new_universe = frigg::makeShared<Universe>(*kernelAlloc);
	
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				UniverseDescriptor(frigg::move(new_universe)));
	}

	return kHelErrNone;
}

HelError helTransferDescriptor(HelHandle handle, HelHandle universe_handle,
		HelHandle *out_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	AnyDescriptor descriptor;
	frigg::SharedPtr<Universe> universe;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard lock(&this_universe->lock);

		auto descriptor_it = this_universe->getDescriptor(lock, handle);
		if(!descriptor_it)
			return kHelErrNoDescriptor;
		descriptor = *descriptor_it;
		
		if(universe_handle == kHelThisUniverse) {
			universe = this_universe.toShared();
		}else{
			auto universe_it = this_universe->getDescriptor(lock, universe_handle);
			if(!universe_it)
				return kHelErrNoDescriptor;
			if(!universe_it->is<UniverseDescriptor>())
				return kHelErrBadDescriptor;
			universe = universe_it->get<UniverseDescriptor>().universe;
		}
	}
	
	// TODO: make sure the descriptor is copyable.

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard lock(&universe->lock);

		*out_handle = universe->attachDescriptor(lock, frigg::move(descriptor));
	}
	return kHelErrNone;
}

HelError helDescriptorInfo(HelHandle handle, HelDescriptorInfo *info) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	auto irq_lock = frigg::guard(&irqMutex());
	Universe::Guard universe_guard(&this_universe->lock);

	auto wrapper = this_universe->getDescriptor(universe_guard, handle);
	if(!wrapper)
		return kHelErrNoDescriptor;
	switch(wrapper->tag()) {
	default:
		assert(!"Illegal descriptor");
	}

	return kHelErrNone;
}

HelError helGetCredentials(HelHandle handle, uint32_t flags, char *credentials) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	assert(!flags);
	
	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	writeUserMemory(credentials, thread->credentials(), 16);

	return kHelErrNone;
}

HelError helCloseDescriptor(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto irq_lock = frigg::guard(&irqMutex());
	Universe::Guard universe_guard(&this_universe->lock);

	if(!this_universe->detachDescriptor(universe_guard, handle))
		return kHelErrNoDescriptor;

	return kHelErrNone;
}

HelError helCreateQueue(HelQueue *head, uint32_t flags, HelHandle *handle) {
	assert(!flags);
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto queue = frigg::makeShared<UserQueue>(*kernelAlloc,
			this_thread->getAddressSpace().toShared(), head);
	queue->setupSelfPtr(queue);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				QueueDescriptor(frigg::move(queue)));
	}

	return kHelErrNone;
}

HelError helSetupChunk(HelHandle queue_handle, int index, HelChunk *chunk, uint32_t flags) {
	assert(!flags);
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<UserQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);
		
		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	queue->setupChunk(index, this_thread->getAddressSpace().toShared(), chunk);

	return kHelErrNone;
}

HelError helCancelAsync(HelHandle handle, uint64_t async_id) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<UserQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);
		
		auto queue_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	queue->cancel(async_id);

	return kHelErrNone;
}

HelError helAllocateMemory(size_t size, uint32_t flags, HelHandle *handle) {
	assert(size > 0);
	assert(size % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

//	auto pressure = physicalAllocator->numUsedPages() * kPageSize;
//	frigg::infoLogger() << "Allocate " << (void *)size
//			<< ", sum of allocated memory: " << (void *)pressure << frigg::endLog;

	frigg::SharedPtr<Memory> memory;
	if(flags & kHelAllocContinuous) {
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size, size, kPageSize);
	}else if(flags & kHelAllocOnDemand) {
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size);
	}else{
		// TODO: 
		memory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size);
	}
	
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryBundleDescriptor(frigg::move(memory)));
	}

	return kHelErrNone;
}

HelError helResizeMemory(HelHandle handle, size_t new_size) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryBundleDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryBundleDescriptor>().memory;
	}

	memory->resize(new_size);

	return kHelErrNone;
}

HelError helCreateManagedMemory(size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle) {
	(void)flags;
	assert(size > 0);
	assert(size % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto managed = frigg::makeShared<ManagedSpace>(*kernelAlloc, size);
	auto backing_memory = frigg::makeShared<BackingMemory>(*kernelAlloc, managed);
	auto frontal_memory = frigg::makeShared<FrontalMemory>(*kernelAlloc, frigg::move(managed));
	
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*backing_handle = this_universe->attachDescriptor(universe_guard,
				MemoryBundleDescriptor(frigg::move(backing_memory)));
		*frontal_handle = this_universe->attachDescriptor(universe_guard,
				MemoryBundleDescriptor(frigg::move(frontal_memory)));
	}

	return kHelErrNone;
}

HelError helAccessPhysical(uintptr_t physical, size_t size, HelHandle *handle) {
	assert((physical % kPageSize) == 0);
	assert((size % kPageSize) == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto memory = frigg::makeShared<HardwareMemory>(*kernelAlloc, physical, size);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				MemoryBundleDescriptor(frigg::move(memory)));
	}

	return kHelErrNone;
}

HelError helCreateSliceView(HelHandle bundle_handle,
		uintptr_t offset, size_t size, uint32_t flags, HelHandle *handle) {
	assert(!flags);
	assert((offset % kPageSize) == 0);
	assert((size % kPageSize) == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> bundle;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, bundle_handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryBundleDescriptor>())
			return kHelErrBadDescriptor;
		bundle = wrapper->get<MemoryBundleDescriptor>().memory;
	}

	auto view = frigg::makeShared<ExteriorBundleView>(*kernelAlloc,
			frigg::move(bundle), offset, size);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				VirtualViewDescriptor(frigg::move(view)));
	}

	return kHelErrNone;
}

HelError helCreateSpace(HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	auto space = frigg::makeShared<AddressSpace>(*kernelAlloc);
	space->setupDefaultMappings();
	
	auto irq_lock = frigg::guard(&irqMutex());
	Universe::Guard universe_guard(&this_universe->lock);

	*handle = this_universe->attachDescriptor(universe_guard,
			AddressSpaceDescriptor(frigg::move(space)));

	return kHelErrNone;
}

HelError helForkSpace(HelHandle handle, HelHandle *forked_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		if(handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	ForkNode node;
	auto done = space->fork(&node);
	assert(done);

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*forked_handle = this_universe->attachDescriptor(universe_guard,
				AddressSpaceDescriptor(node.forkedSpace()));
	}

	return kHelErrNone;
}

HelError helMapMemory(HelHandle memory_handle, HelHandle space_handle,
		void *pointer, uintptr_t offset, size_t length, uint32_t flags, void **actual_pointer) {
	if(length == 0)
		return kHelErrIllegalArgs;
	if((uintptr_t)pointer % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(offset % kPageSize != 0)
		return kHelErrIllegalArgs;
	if(length % kPageSize != 0)
		return kHelErrIllegalArgs;

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<VirtualView> view;
	frigg::SharedPtr<AddressSpace> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, memory_handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(memory_wrapper->is<VirtualViewDescriptor>()) {
			view = memory_wrapper->get<VirtualViewDescriptor>().view;
		}else if(memory_wrapper->is<MemoryBundleDescriptor>()) {
			auto memory = memory_wrapper->get<MemoryBundleDescriptor>().memory;
			auto bundle_length = memory->getLength();
			view = frigg::makeShared<ExteriorBundleView>(*kernelAlloc,
					frigg::move(memory), 0, bundle_length);
		}else{
			return kHelErrBadDescriptor;
		}
		
		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}

	// TODO: check proper alignment

	uint32_t map_flags = 0;
	if(pointer != nullptr) {
		map_flags |= AddressSpace::kMapFixed;
	}else{
		map_flags |= AddressSpace::kMapPreferTop;
	}

	if(flags & kHelMapProtRead)
		map_flags |= AddressSpace::kMapProtRead;
	if(flags & kHelMapProtWrite)
		map_flags |= AddressSpace::kMapProtWrite;
	if(flags & kHelMapProtExecute)
		map_flags |= AddressSpace::kMapProtExecute;

	if(flags & kHelMapDropAtFork) {
		map_flags |= AddressSpace::kMapDropAtFork;
	}else if(flags & kHelMapShareAtFork) {
		map_flags |= AddressSpace::kMapShareAtFork;
	}else if(flags & kHelMapCopyOnWriteAtFork) {
		map_flags |= AddressSpace::kMapCopyOnWriteAtFork;
	}

	if(flags & kHelMapDontRequireBacking)
		map_flags |= AddressSpace::kMapDontRequireBacking;

	Error error;
	VirtualAddr actual_address;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&space->lock);

		error = space->map(space_guard, view, (VirtualAddr)pointer, offset, length,
				map_flags, &actual_address);
	}

	if(error == kErrBufferTooSmall) {
		return kHelErrBufferTooSmall;
	}else{
		assert(!error);
		*actual_pointer = (void *)actual_address;
		return kHelErrNone;
	}
}

HelError helUnmapMemory(HelHandle space_handle, void *pointer, size_t length) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}
	
	auto closure = frigg::construct<AddressUnmapNode>(*kernelAlloc);
	{
		auto irq_lock = frigg::guard(&irqMutex());
		AddressSpace::Guard space_guard(&space->lock);

		space->unmap(space_guard, (VirtualAddr)pointer, length, closure);
	}

	return kHelErrNone;
}

HelError helPointerPhysical(void *pointer, uintptr_t *physical) {
	auto this_thread = getCurrentThread();
	
	frigg::SharedPtr<AddressSpace> space = this_thread->getAddressSpace().toShared();

	// FIXME: The physical page can change after we destruct the accessor!
	// We need a better hel API to properly handle that case.
	AcquireNode node;

	auto disp = (reinterpret_cast<uintptr_t>(pointer) & (kPageSize - 1));
	auto accessor = ForeignSpaceAccessor{frigg::move(space),
			reinterpret_cast<char *>(pointer) - disp, kPageSize};
	node.setup(nullptr);
	auto acq = accessor.acquire(&node);
	assert(acq);

	auto page_physical = accessor.getPhysical(0);

	*physical = page_physical + disp;

	return kHelErrNone;
}

HelError helLoadForeign(HelHandle handle, uintptr_t address,
		size_t length, void *buffer) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(wrapper->is<AddressSpaceDescriptor>()) {
			space = wrapper->get<AddressSpaceDescriptor>().space;
		}else if(wrapper->is<ThreadDescriptor>()) {
			auto thread = wrapper->get<ThreadDescriptor>().thread;
			space = thread->getAddressSpace().toShared();
		}else{
			return kHelErrBadDescriptor;
		}
	}

	AcquireNode node;

	// TODO: This enableUserAccess() should be replaced by a writeUserMemory().
	auto accessor = ForeignSpaceAccessor{frigg::move(space),
			(void *)address, length};
	node.setup(nullptr);
	auto acq = accessor.acquire(&node);
	assert(acq);

	enableUserAccess();
	accessor.load(0, buffer, length);
	disableUserAccess();

	return kHelErrNone;
}

HelError helStoreForeign(HelHandle handle, uintptr_t address,
		size_t length, const void *buffer) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<AddressSpace> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(wrapper->is<AddressSpaceDescriptor>()) {
			space = wrapper->get<AddressSpaceDescriptor>().space;
		}else if(wrapper->is<ThreadDescriptor>()) {
			auto thread = wrapper->get<ThreadDescriptor>().thread;
			space = thread->getAddressSpace().toShared();
		}else{
			return kHelErrBadDescriptor;
		}
	}

	AcquireNode node;

	// TODO: This enableUserAccess() should be replaced by a readUserMemory().
	auto accessor = ForeignSpaceAccessor{frigg::move(space),
			(void *)address, length};
	node.setup(nullptr);
	auto acq = accessor.acquire(&node);
	assert(acq);

	enableUserAccess();
	auto error = accessor.write(0, buffer, length);
	assert(!error);
	disableUserAccess();

	return kHelErrNone;
}

HelError helMemoryInfo(HelHandle handle, size_t *size) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<MemoryBundleDescriptor>())
			return kHelErrBadDescriptor;
		memory = wrapper->get<MemoryBundleDescriptor>().memory;
	}

	*size = memory->getLength();
	return kHelErrNone;
}

HelError helSubmitManageMemory(HelHandle handle, HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Memory> memory;
	frigg::SharedPtr<UserQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryBundleDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryBundleDescriptor>().memory;
		
		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	struct Closure : QueueNode {
		Closure()
		: ipcSource{&helResult, sizeof(HelManageResult), nullptr} {
			setupSource(&ipcSource);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

		frigg::SharedPtr<UserQueue> ipcQueue;
		Worklet worklet;
		ManageBase manage;
		QueueSource ipcSource;

		HelManageResult helResult;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static void managed(Worklet *base) {
			auto closure = frg::container_of(base, &Closure::worklet);
			closure->helResult = HelManageResult{translateError(closure->manage.error()), 0,
					closure->manage.offset(), closure->manage.size()};
			closure->ipcQueue->submit(closure);
		}
	};

	closure->ipcQueue = frigg::move(queue);
	closure->setup(this_thread->associatedWorkQueue());
	closure->setupContext(context);

	closure->worklet.setup(&Ops::managed, getCurrentThread()->associatedWorkQueue());
	closure->manage.setup(&closure->worklet);
	memory->submitManage(&closure->manage);

	return kHelErrNone;
}

HelError helCompleteLoad(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryBundleDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryBundleDescriptor>().memory;
	}


	memory->completeLoad(offset, length);

	return kHelErrNone;
}

HelError helSubmitLockMemory(HelHandle handle, uintptr_t offset, size_t size,
		HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Memory> memory;
	frigg::SharedPtr<UserQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryBundleDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryBundleDescriptor>().memory;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	struct Closure : QueueNode {
		Closure()
		: ipcSource{&helResult, sizeof(HelSimpleResult), nullptr} {
			setupSource(&ipcSource);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

		frigg::SharedPtr<UserQueue> ipcQueue;
		Worklet worklet;
		InitiateBase initiate;
		QueueSource ipcSource;

		HelSimpleResult helResult;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static void initiated(Worklet *base) {
			auto closure = frg::container_of(base, &Closure::worklet);
			closure->helResult = HelSimpleResult{translateError(closure->initiate.error()), 0};
			closure->ipcQueue->submit(closure);
		}
	};

	closure->ipcQueue = frigg::move(queue);
	closure->setup(this_thread->associatedWorkQueue());
	closure->setupContext(context);

	closure->worklet.setup(&Ops::initiated, getCurrentThread()->associatedWorkQueue());
	closure->initiate.setup(offset, size, &closure->worklet);
	memory->submitInitiateLoad(&closure->initiate);

	return kHelErrNone;
}

HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length) {
	assert(offset % kPageSize == 0 && length % kPageSize == 0);

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<Memory> memory;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto memory_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!memory_wrapper)
			return kHelErrNoDescriptor;
		if(!memory_wrapper->is<MemoryBundleDescriptor>())
			return kHelErrBadDescriptor;
		memory = memory_wrapper->get<MemoryBundleDescriptor>().memory;
	}

/*	auto handle_load = frigg::makeShared<AsyncInitiateLoad>(*kernelAlloc,
			NullCompleter(), offset, length);
	{
		// TODO: protect memory object with a guard
		memory->submitInitiateLoad(frigg::move(handle_load));
	}*/
	
	return kHelErrNone;
}

std::atomic<unsigned int> globalNextCpu = 0;

HelError helCreateThread(HelHandle universe_handle, HelHandle space_handle,
		int abi, void *ip, void *sp, uint32_t flags, HelHandle *handle) {
	(void)abi;
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	if(flags & ~(kHelThreadExclusive | kHelThreadTrapsAreFatal
			| kHelThreadStopped))
		return kHelErrIllegalArgs;

	frigg::SharedPtr<Universe> universe;
	frigg::SharedPtr<AddressSpace> space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);
		
		if(universe_handle == kHelNullHandle) {
			universe = this_thread->getUniverse().toShared();
		}else{
			auto universe_wrapper = this_universe->getDescriptor(universe_guard, universe_handle);
			if(!universe_wrapper)
				return kHelErrNoDescriptor;
			if(!universe_wrapper->is<UniverseDescriptor>())
				return kHelErrBadDescriptor;
			universe = universe_wrapper->get<UniverseDescriptor>().universe;
		}

		if(space_handle == kHelNullHandle) {
			space = this_thread->getAddressSpace().toShared();
		}else{
			auto space_wrapper = this_universe->getDescriptor(universe_guard, space_handle);
			if(!space_wrapper)
				return kHelErrNoDescriptor;
			if(!space_wrapper->is<AddressSpaceDescriptor>())
				return kHelErrBadDescriptor;
			space = space_wrapper->get<AddressSpaceDescriptor>().space;
		}
	}
	
	AbiParameters params;
	params.ip = (uintptr_t)ip;
	params.sp = (uintptr_t)sp;

	auto new_thread = Thread::create(frigg::move(universe), frigg::move(space), params);
	new_thread->self = new_thread;
	if(flags & kHelThreadExclusive)
		new_thread->flags |= Thread::kFlagExclusive;
	if(flags & kHelThreadTrapsAreFatal)
		new_thread->flags |= Thread::kFlagTrapsAreFatal;

	// Adding a large prime (coprime to getCpuCount()) should yield a good distribution.
	auto cpu = globalNextCpu.fetch_add(4099, std::memory_order_relaxed) % getCpuCount();
//	frigg::infoLogger() << "thor: New thread on CPU #" << cpu << frigg::endLog;
	Scheduler::associate(new_thread.get(), &getCpuData(cpu)->scheduler);
//	Scheduler::associate(new_thread.get(), localScheduler());
	if(!(flags & kHelThreadStopped))
		Thread::resumeOther(new_thread);

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				ThreadDescriptor(frigg::move(new_thread)));
	}

	return kHelErrNone;
}

HelError helSetPriority(HelHandle handle, int priority) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	if(handle == kHelThisThread) {
		thread = this_thread.toShared();
	}else{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	Scheduler::setPriority(thread.get(), priority);

	return kHelErrNone;
}

HelError helYield() {
	Thread::deferCurrent();

	return kHelErrNone;
}

HelError helSubmitObserve(HelHandle handle, uint64_t in_seq,
		HelHandle queue_handle, uintptr_t context) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	frigg::SharedPtr<UserQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}
	
	PostEvent<ObserveThreadWriter> functor{frigg::move(queue), context};
	thread->submitObserve(in_seq, frigg::move(functor));

	return kHelErrNone;
}

HelError helKillThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}	

	Thread::killOther(thread);

	return kHelErrNone;
}

HelError helInterruptThread(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}	

	Thread::interruptOther(thread);

	return kHelErrNone;
}

HelError helResume(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}	

	Thread::resumeOther(thread);

	return kHelErrNone;
}

HelError helLoadRegisters(HelHandle handle, int set, void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}

	// TODO: Make sure that the thread is actually suspenend!

	if(set == kHelRegsProgram) {
		uintptr_t regs[2];
		regs[0] = *thread->_executor.ip();
		regs[1] = *thread->_executor.sp();
		writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2);
	}else if(set == kHelRegsGeneral) {
		uintptr_t regs[15];
		regs[0] = thread->_executor.general()->rax;
		regs[1] = thread->_executor.general()->rbx;
		regs[2] = thread->_executor.general()->rcx;
		regs[3] = thread->_executor.general()->rdx;
		regs[4] = thread->_executor.general()->rdi;
		regs[5] = thread->_executor.general()->rsi;
		regs[6] = thread->_executor.general()->r8;
		regs[7] = thread->_executor.general()->r9;
		regs[8] = thread->_executor.general()->r10;
		regs[9] = thread->_executor.general()->r11;
		regs[10] = thread->_executor.general()->r12;
		regs[11] = thread->_executor.general()->r13;
		regs[12] = thread->_executor.general()->r14;
		regs[13] = thread->_executor.general()->r15;
		regs[14] = thread->_executor.general()->rbp;
		writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 15);
	}else if(set == kHelRegsThread) {
		uintptr_t regs[2];
		regs[0] = thread->_executor.general()->clientFs;
		regs[1] = thread->_executor.general()->clientGs;
		writeUserArray(reinterpret_cast<uintptr_t *>(image), regs, 2);
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helStoreRegisters(HelHandle handle, int set, const void *image) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<Thread> thread;
	if(handle == kHelThisThread) {
		// FIXME: Properly handle this below.
		thread = this_thread.toShared();
	}else{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto thread_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!thread_wrapper)
			return kHelErrNoDescriptor;
		if(!thread_wrapper->is<ThreadDescriptor>())
			return kHelErrBadDescriptor;
		thread = thread_wrapper->get<ThreadDescriptor>().thread;
	}
	
	// TODO: Make sure that the thread is actually suspenend!

	if(set == kHelRegsProgram) {
		uintptr_t regs[2];
		readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2);
		*thread->_executor.ip() = regs[0];
		*thread->_executor.sp() = regs[1];
	}else if(set == kHelRegsGeneral) {
		uintptr_t regs[15];
		readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 15);
		thread->_executor.general()->rax = regs[0];
		thread->_executor.general()->rbx = regs[1];
		thread->_executor.general()->rcx = regs[2];
		thread->_executor.general()->rdx = regs[3];
		thread->_executor.general()->rdi = regs[4];
		thread->_executor.general()->rsi = regs[5];
		thread->_executor.general()->r8 = regs[6];
		thread->_executor.general()->r9 = regs[7];
		thread->_executor.general()->r10 = regs[8];
		thread->_executor.general()->r11 = regs[9];
		thread->_executor.general()->r12 = regs[10];
		thread->_executor.general()->r13 = regs[11];
		thread->_executor.general()->r14 = regs[12];
		thread->_executor.general()->r15 = regs[13];
		thread->_executor.general()->rbp = regs[14];
	}else if(set == kHelRegsThread) {
		uintptr_t regs[2];
		readUserArray(reinterpret_cast<const uintptr_t *>(image), regs, 2);
		thread->_executor.general()->clientFs = regs[0];
		thread->_executor.general()->clientGs = regs[1];
	}else if(set == kHelRegsDebug) {
		// FIXME: Make those registers thread-specific.
		auto reg = readUserObject(reinterpret_cast<uint32_t *const *>(image));
		breakOnWrite(reg);
	}else{
		return kHelErrIllegalArgs;
	}

	return kHelErrNone;
}

HelError helWriteFsBase(void *pointer) {
	frigg::arch_x86::wrmsr(frigg::arch_x86::kMsrIndexFsBase, (uintptr_t)pointer);
	return kHelErrNone;
}

HelError helGetClock(uint64_t *counter) {
	*counter = systemClockSource()->currentNanos();
	return kHelErrNone;
}

HelError helSubmitAwaitClock(uint64_t counter, HelHandle queue_handle, uintptr_t context,
		uint64_t *async_id) {
	struct Closure : CancelNode, PrecisionTimerNode, QueueNode {
		static void issue(uint64_t nanos, frigg::SharedPtr<UserQueue> queue,
				uintptr_t context, uint64_t *async_id) {
			auto node = frigg::construct<Closure>(*kernelAlloc, nanos,
					frigg::move(queue), context);
			node->queue->issue(node);
			*async_id = node->asyncId();
			generalTimerEngine()->installTimer(node);
		}

		static void elapsed(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &Closure::worklet);
			closure->finalizeCancel();
			closure->queue->submit(closure);
		}

		explicit Closure(uint64_t nanos, frigg::SharedPtr<UserQueue> the_queue,
				uintptr_t context)
		: queue{frigg::move(the_queue)},
				source{&result, sizeof(HelSimpleResult), nullptr},
				result{translateError(kErrSuccess), 0} {
			thread = getCurrentThread().get();
			QueueNode::setup(thread->associatedWorkQueue());
			setupContext(context);
			setupSource(&source);
			
			worklet.setup(&Closure::elapsed, thread->associatedWorkQueue());
			PrecisionTimerNode::setup(nanos, &worklet);
		}

		void handleCancel() override {
			cancelTimer();
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

		Thread *thread;
		Worklet worklet;
		frigg::SharedPtr<UserQueue> queue;
		QueueSource source;
		HelSimpleResult result;
	};

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<UserQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	Closure::issue(counter, frigg::move(queue), context, async_id);

	return kHelErrNone;
}

HelError helCreateStream(HelHandle *lane1_handle, HelHandle *lane2_handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	auto lanes = createStream();
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*lane1_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(frigg::move(lanes.get<0>())));
		*lane2_handle = this_universe->attachDescriptor(universe_guard,
				LaneDescriptor(frigg::move(lanes.get<1>())));
	}

	return kHelErrNone;
}

HelError helSubmitAsync(HelHandle handle, const HelAction *actions, size_t count,
		HelHandle queue_handle, uintptr_t context, uint32_t flags) {
	(void)flags;
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	// TODO: check userspace page access rights

	LaneHandle lane;
	frigg::SharedPtr<UserQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		if(handle == kHelThisThread) {
			lane = this_thread->inferiorLane();
		}else{
			auto wrapper = this_universe->getDescriptor(universe_guard, handle);
			if(!wrapper)
				return kHelErrNoDescriptor;
			if(wrapper->is<LaneDescriptor>()) {
				lane = wrapper->get<LaneDescriptor>().handle;
			}else if(wrapper->is<ThreadDescriptor>()) {
				lane = wrapper->get<ThreadDescriptor>().thread->superiorLane();
			}else{
				return kHelErrBadDescriptor;
			}
		}

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	struct Item {
		StreamNode transmit;
		frigg::UniqueMemory<KernelAlloc> buffer;
		QueueSource mainSource;
		QueueSource dataSource;
		union {
			HelSimpleResult helSimpleResult;
			HelHandleResult helHandleResult;
			HelCredentialsResult helCredentialsResult;
			HelInlineResultNoFlex helInlineResult;
			HelLengthResult helLengthResult;
		};
	};

	struct Closure : QueueNode {
		void complete() override {
			// TODO: Turn items into a unique_ptr.
			frigg::destructN(*kernelAlloc, items, count);
			frigg::destruct(*kernelAlloc, this);
		}
		
		size_t count;
		frigg::WeakPtr<Universe> weakUniverse;
		frigg::SharedPtr<UserQueue> ipcQueue;

		Worklet worklet;
		StreamPacket packet;
		Item *items;
	} *closure = frigg::construct<Closure>(*kernelAlloc);

	struct Ops {
		static void transmitted(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &Closure::worklet);

			QueueSource *tail = nullptr;
			auto link = [&] (QueueSource *source) {
				if(tail)
					tail->link = source;
				tail = source;
			};

			for(size_t i = 0; i < closure->count; i++) {
				auto item = &closure->items[i];
				if(item->transmit.tag() == kTagOffer) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagAccept) {
					// TODO: This condition should be replaced. Just test if lane is valid.
					HelHandle handle = kHelNullHandle;
					if(!item->transmit.error()) {
						auto universe = closure->weakUniverse.grab();
						assert(universe);

						auto irq_lock = frigg::guard(&irqMutex());
						Universe::Guard lock(&universe->lock);

						handle = universe->attachDescriptor(lock,
								LaneDescriptor{item->transmit.lane()});
					}
					
					item->helHandleResult = {translateError(item->transmit.error()), 0, handle};
					item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagImbueCredentials) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagExtractCredentials) {
					item->helCredentialsResult = {translateError(item->transmit.error()), 0};
					memcpy(item->helCredentialsResult.credentials,
							item->transmit.credentials().data(), 16);
					item->mainSource.setup(&item->helCredentialsResult,
							sizeof(HelCredentialsResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagSendFromBuffer) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagRecvInline) {
					item->buffer = item->transmit.transmitBuffer();

					item->helInlineResult = {translateError(item->transmit.error()),
							0, item->buffer.size()};
					item->mainSource.setup(&item->helInlineResult, sizeof(HelInlineResultNoFlex));
					item->dataSource.setup(item->buffer.data(), item->buffer.size());
					link(&item->mainSource);
					link(&item->dataSource);
				}else if(item->transmit.tag() == kTagRecvToBuffer) {
					item->helLengthResult = {translateError(item->transmit.error()),
							0, item->transmit.actualLength()};
					item->mainSource.setup(&item->helLengthResult, sizeof(HelLengthResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagPushDescriptor) {
					item->helSimpleResult = {translateError(item->transmit.error()), 0};
					item->mainSource.setup(&item->helSimpleResult, sizeof(HelSimpleResult));
					link(&item->mainSource);
				}else if(item->transmit.tag() == kTagPullDescriptor) {
					// TODO: This condition should be replaced. Just test if lane is valid.
					HelHandle handle = kHelNullHandle;
					if(!item->transmit.error()) {
						auto universe = closure->weakUniverse.grab();
						assert(universe);

						auto irq_lock = frigg::guard(&irqMutex());
						Universe::Guard lock(&universe->lock);

						handle = universe->attachDescriptor(lock, item->transmit.descriptor());
					}
					
					item->helHandleResult = {translateError(item->transmit.error()), 0, handle};
					item->mainSource.setup(&item->helHandleResult, sizeof(HelHandleResult));
					link(&item->mainSource);
				}else{
					frigg::panicLogger() << "thor: Unexpected transmission tag" << frigg::endLog;
				}
			}

			assert(closure->count);
			closure->setupSource(&closure->items[0].mainSource);
			closure->ipcQueue->submit(closure);
		}
	};
	
	closure->count = count;
	closure->weakUniverse = this_universe.toWeak();
	closure->ipcQueue = frigg::move(queue);
	
	closure->worklet.setup(&Ops::transmitted, WorkQueue::localQueue());
	closure->packet.setup(count, &closure->worklet);
	closure->setup(WorkQueue::localQueue());
	closure->setupContext(context);
	closure->items = frigg::constructN<Item>(*kernelAlloc, count);

	frigg::Vector<LaneHandle, KernelAlloc> stack(*kernelAlloc);
	stack.push(frigg::move(lane));

	size_t i = 0;
	while(!stack.empty()) {
		assert(i < count);
		HelAction action = readUserObject(actions + i++);

		auto target = stack.back();
		if(!(action.flags & kHelItemChain))
			stack.pop();

		switch(action.type) {
		case kHelActionOffer: {
			closure->items[i - 1].transmit.setup(kTagOffer, &closure->packet);
			LaneHandle branch = target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);

			if(action.flags & kHelItemAncillary)
				stack.push(branch);
		} break;
		case kHelActionAccept: {
			closure->items[i - 1].transmit.setup(kTagAccept, &closure->packet);
			LaneHandle branch = target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);

			if(action.flags & kHelItemAncillary)
				stack.push(branch);
		} break;
		case kHelActionImbueCredentials: {
			closure->items[i - 1].transmit.setup(kTagImbueCredentials, &closure->packet);
			memcpy(closure->items[i - 1].transmit._inCredentials.data(),
					this_thread->credentials(), 16);
			target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);
		} break;
		case kHelActionExtractCredentials: {
			closure->items[i - 1].transmit.setup(kTagExtractCredentials, &closure->packet);
			target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);
		} break;
		case kHelActionSendFromBuffer: {
			frigg::UniqueMemory<KernelAlloc> buffer(*kernelAlloc, action.length);
			readUserMemory(buffer.data(), action.buffer, action.length);

			closure->items[i - 1].transmit.setup(kTagSendFromBuffer, &closure->packet);
			closure->items[i - 1].transmit._inBuffer = frigg::move(buffer);
			target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);
		} break;
		case kHelActionSendFromBufferSg: {
			size_t length = 0;
			auto sglist = reinterpret_cast<HelSgItem *>(action.buffer);
			for(size_t j = 0; j < action.length; j++) {
				auto item = readUserObject(sglist + j);
				length += item.length;
			}

			frigg::UniqueMemory<KernelAlloc> buffer(*kernelAlloc, length);
			size_t offset = 0;
			for(size_t j = 0; j < action.length; j++) {
				auto item = readUserObject(sglist + j);
				readUserMemory(reinterpret_cast<char *>(buffer.data()) + offset,
						reinterpret_cast<char *>(item.buffer), item.length);
				offset += item.length;
			}

			closure->items[i - 1].transmit.setup(kTagSendFromBuffer, &closure->packet);
			closure->items[i - 1].transmit._inBuffer = frigg::move(buffer);
			target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);
		} break;
		case kHelActionRecvInline: {
			auto space = this_thread->getAddressSpace().toShared();
			closure->items[i - 1].transmit.setup(kTagRecvInline, &closure->packet);
			target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);
		} break;
		case kHelActionRecvToBuffer: {
			auto space = this_thread->getAddressSpace().toShared();
			AcquireNode node;
			auto accessor = ForeignSpaceAccessor{frigg::move(space),
					action.buffer, action.length};
			node.setup(nullptr);
			auto acq = accessor.acquire(&node);
			assert(acq);

			closure->items[i - 1].transmit.setup(kTagRecvToBuffer, &closure->packet);
			closure->items[i - 1].transmit._inAccessor = frigg::move(accessor);
			target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);
		} break;
		case kHelActionPushDescriptor: {
			AnyDescriptor operand;
			{
				auto irq_lock = frigg::guard(&irqMutex());
				Universe::Guard universe_guard(&this_universe->lock);

				auto wrapper = this_universe->getDescriptor(universe_guard, action.handle);
				if(!wrapper)
					return kHelErrNoDescriptor;
				operand = *wrapper;
			}

			closure->items[i - 1].transmit.setup(kTagPushDescriptor, &closure->packet);
			closure->items[i - 1].transmit._inDescriptor = frigg::move(operand);
			target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);
		} break;
		case kHelActionPullDescriptor: {
			closure->items[i - 1].transmit.setup(kTagPullDescriptor, &closure->packet);
			target.getStream()->transmit(target.getLane(),
					&closure->items[i - 1].transmit);
		} break;
		default:
			assert(!"Fix error handling here");
		}
	}
	assert(i == count);

	return kHelErrNone;
}

HelError helShutdownLane(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	LaneHandle lane;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<LaneDescriptor>())
			return kHelErrBadDescriptor;
		lane = wrapper->get<LaneDescriptor>().handle;
	}

	lane.getStream()->shutdownLane(lane.getLane());

	return kHelErrNone;
}

HelError helFutexWait(int *pointer, int expected) {
	auto this_thread = getCurrentThread();
	auto space = this_thread->getAddressSpace();

	struct Closure {
		ThreadBlocker blocker;
		Worklet worklet;
		FutexNode futex;
	} closure;

	// TODO: Support physical (i.e. non-private) futexes.
	closure.worklet.setup([] (Worklet *base) {
		auto closure = frg::container_of(base, &Closure::worklet);
		Thread::unblockOther(&closure->blocker);
	}, this_thread->associatedWorkQueue());
	closure.futex.setup(&closure.worklet);
	closure.blocker.setup();
	space->futexSpace.submitWait(VirtualAddr(pointer), [&] () -> bool {
		enableUserAccess();
		auto v = __atomic_load_n(pointer, __ATOMIC_RELAXED);
		disableUserAccess();
		return expected == v;
	}, &closure.futex);

/*
	frigg::infoLogger() << "thor: "
			<< " " << this_thread->credentials()[0] << " " << this_thread->credentials()[1]
			<< " " << this_thread->credentials()[2] << " " << this_thread->credentials()[3]
			<< " " << this_thread->credentials()[4] << " " << this_thread->credentials()[5]
			<< " " << this_thread->credentials()[6] << " " << this_thread->credentials()[7]
			<< " " << this_thread->credentials()[8] << " " << this_thread->credentials()[9]
			<< " " << this_thread->credentials()[10] << " " << this_thread->credentials()[11]
			<< " " << this_thread->credentials()[12] << " " << this_thread->credentials()[13]
			<< " " << this_thread->credentials()[14] << " " << this_thread->credentials()[15]
			<< " Thread blocked on futex" << frigg::endLog;
*/
	
	Thread::blockCurrent(&closure.blocker);

	return kHelErrNone;
}

HelError helFutexWake(int *pointer) {
	auto this_thread = getCurrentThread();
	auto space = this_thread->getAddressSpace();

	{
		// TODO: Support physical (i.e. non-private) futexes.
		space->futexSpace.wake(VirtualAddr(pointer));
	}

	return kHelErrNone;
}

HelError helAccessIrq(int number, HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	auto irq = frigg::makeShared<IrqObject>(*kernelAlloc,
			frigg::String<KernelAlloc>{*kernelAlloc, "generic-irq-object"});
	IrqPin::attachSink(getGlobalSystemIrq(number), irq.get());

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				IrqDescriptor(frigg::move(irq)));
	}

	return kHelErrNone;
}

HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence) {
	assert(!(flags & ~(kHelAckAcknowledge | kHelAckNack | kHelAckKick)));

	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	auto mode = flags & (kHelAckAcknowledge | kHelAckNack | kHelAckKick);
	if(mode != kHelAckAcknowledge && mode != kHelAckNack && mode != kHelAckKick)
		return kHelErrIllegalArgs;
	
	frigg::SharedPtr<IrqObject> irq;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto irq_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq = irq_wrapper->get<IrqDescriptor>().irq;
	}

	Error error;
	if(mode == kHelAckAcknowledge) {
		error = IrqPin::ackSink(irq.get(), sequence);
	}else if(mode == kHelAckNack) {
		error = IrqPin::nackSink(irq.get(), sequence);
	}else{
 		assert(mode == kHelAckKick);
		error = IrqPin::kickSink(irq.get());
	}

	if(error == kErrIllegalArgs) {
		return kHelErrIllegalArgs;
	}else{
		assert(!error);
		return kHelErrNone;
	}
}

HelError helSubmitAwaitEvent(HelHandle handle, uint64_t sequence,
		HelHandle queue_handle, uintptr_t context) {
	struct Closure : QueueNode {
		static void issue(frigg::SharedPtr<IrqObject> irq, uint64_t sequence,
				frigg::SharedPtr<UserQueue> queue, intptr_t context) {
			auto closure = frigg::construct<Closure>(*kernelAlloc,
					frigg::move(queue), context);
			irq->submitAwait(&closure->irqNode, sequence);
		}
		
		static void awaited(Worklet *worklet) {
			auto closure = frg::container_of(worklet, &Closure::worklet);
			closure->result.error = translateError(closure->irqNode.error());
			closure->result.sequence = closure->irqNode.sequence();
			closure->_queue->submit(closure);
		}

	public:
		explicit Closure(frigg::SharedPtr<UserQueue> the_queue, uintptr_t context)
		: _queue{frigg::move(the_queue)},
				source{&result, sizeof(HelEventResult), nullptr} {
			thread = getCurrentThread().get();
			setup(thread->associatedWorkQueue());
			setupContext(context);
			setupSource(&source);
			worklet.setup(&Closure::awaited, thread->associatedWorkQueue());
			irqNode.setup(&worklet);
		}

		void complete() override {
			frigg::destruct(*kernelAlloc, this);
		}

	private:
		Thread *thread;
		Worklet worklet;
		AwaitIrqNode irqNode;
		frigg::SharedPtr<UserQueue> _queue;
		QueueSource source;
		HelEventResult result;
	};
	
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();

	frigg::SharedPtr<IrqObject> irq;
	frigg::SharedPtr<UserQueue> queue;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto irq_wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!irq_wrapper)
			return kHelErrNoDescriptor;
		if(!irq_wrapper->is<IrqDescriptor>())
			return kHelErrBadDescriptor;
		irq = irq_wrapper->get<IrqDescriptor>().irq;

		auto queue_wrapper = this_universe->getDescriptor(universe_guard, queue_handle);
		if(!queue_wrapper)
			return kHelErrNoDescriptor;
		if(!queue_wrapper->is<QueueDescriptor>())
			return kHelErrBadDescriptor;
		queue = queue_wrapper->get<QueueDescriptor>().queue;
	}

	Closure::issue(frigg::move(irq), sequence,
			frigg::move(queue), context);

	return kHelErrNone;
}

HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	// TODO: check userspace page access rights
	auto io_space = frigg::makeShared<IoSpace>(*kernelAlloc);
	for(size_t i = 0; i < num_ports; i++)
		io_space->addPort(readUserObject<uintptr_t>(port_array + i));

	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		*handle = this_universe->attachDescriptor(universe_guard,
				IoDescriptor(frigg::move(io_space)));
	}

	return kHelErrNone;
}

HelError helEnableIo(HelHandle handle) {
	auto this_thread = getCurrentThread();
	auto this_universe = this_thread->getUniverse();
	
	frigg::SharedPtr<IoSpace> io_space;
	{
		auto irq_lock = frigg::guard(&irqMutex());
		Universe::Guard universe_guard(&this_universe->lock);

		auto wrapper = this_universe->getDescriptor(universe_guard, handle);
		if(!wrapper)
			return kHelErrNoDescriptor;
		if(!wrapper->is<IoDescriptor>())
			return kHelErrBadDescriptor;
		io_space = wrapper->get<IoDescriptor>().ioSpace;
	}

	io_space->enableInThread(this_thread);

	return kHelErrNone;
}

HelError helEnableFullIo() {
	auto this_thread = getCurrentThread();

	for(uintptr_t port = 0; port < 0x10000; port++)
		this_thread->getContext().enableIoPort(port);

	return kHelErrNone;
}

