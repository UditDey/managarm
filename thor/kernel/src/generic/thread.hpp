
namespace thor {

enum Fault {
	kFaultNone,
	kFaultBreakpoint
};

struct Thread;

KernelUnsafePtr<Thread> getCurrentThread();

struct Thread : public PlatformExecutor {
friend class ThreadRunControl;
public:
	// wrapper for the function below
	template<typename F>
	static void blockCurrent(F functor) {
		blockCurrent(&functor, [] (void *argument) {
			(*static_cast<F *>(argument))();
		});
	}

	template<typename P>
	static void blockCurrentWhile(P predicate) {
		// optimization: do not acquire the lock for the first test.
		if(!predicate())
			return;

		frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
		while(true) {
			auto guard = frigg::guard(&this_thread->_mutex);
			if(!predicate())
				return;
			_blockLocked(frigg::move(guard));
		}
	}

	// state transitions that apply to the current thread only.
	static void deferCurrent();
	static void blockCurrent(void *argument, void (*function) (void *));
	static void faultCurret(FaultImageAccessor image);
	
	// state transitions that apply to arbitrary threads.
	static void activateOther(frigg::UnsafePtr<Thread> thread);
	static void unblockOther(frigg::UnsafePtr<Thread> thread);
	static void interruptOther(frigg::UnsafePtr<Thread> thread);
	static void resumeOther(frigg::UnsafePtr<Thread> thread);

	// these signals let the thread change its RunState.
	// do not confuse them with POSIX signals!
	enum Signal {
		kSigNone,
		kSigKill
	};

	enum Flags : uint32_t {
		// disables preemption for this thread
		kFlagExclusive = 1,

		// traps kill the process instead of just halting it
		kFlagTrapsAreFatal = 2
	};

	Thread(KernelSharedPtr<Universe> universe,
			KernelSharedPtr<AddressSpace> address_space);
	~Thread();

	Context &getContext();
	KernelUnsafePtr<Universe> getUniverse();
	KernelUnsafePtr<AddressSpace> getAddressSpace();

	void signalKill();
	Signal pendingSignal();

	void transitionToFault();

	void submitObserve(KernelSharedPtr<AsyncObserve> observe);

	uint32_t flags;

private:
	typedef frigg::TicketLock Mutex;

	enum RunState {
		kRunNone,

		// the thread is running on some processor.
		kRunActive,

		// the thread is in the schedule queue but not active on any processor.
		// it may be killed in this state.
		kRunSuspended,

		// like kRunSuspended but the thread must not be killed in this state.
		kRunDeferred,

		// the thread is waiting for progress inside the kernel.
		// it is not scheduled.
		kRunBlocked,

		// the thread is stopped after a fault and is waiting for a
		// userspace watchdog to resume it.
		// it is not scheduled.
		kRunFaulted,

		// the thread was manually stopped from userspace.
		// it is not scheduled.
		kRunInterrupted
	};

	static void _blockLocked(frigg::LockGuard<Mutex> lock);

	Mutex _mutex;

	RunState _runState;

	// number of ticks this thread has been running (i.e. in the active state)
	uint64_t _numTicks;

	// tick in which this thread transitioned to the active state
	uint64_t _activationTick;

	// this is set by signalKill() and queried each time the kernel
	// is ready to process the kill request, e.g. after a syscall finishes.
	Signal _pendingSignal;

	// number of references that keep this thread running.
	// the thread is killed when this counter reaches zero.
	int _runCount;

	Context _context;

	KernelSharedPtr<Universe> _universe;
	KernelSharedPtr<AddressSpace> _addressSpace;

	frigg::IntrusiveSharedLinkedList<
		AsyncObserve,
		&AsyncObserve::processQueueItem
	> _observeQueue;
};

} // namespace thor
