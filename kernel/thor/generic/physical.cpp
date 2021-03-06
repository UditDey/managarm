#include "kernel.hpp"

namespace thor {

static bool logPhysicalAllocs = false;

// --------------------------------------------------------
// SkeletalRegion
// --------------------------------------------------------

frigg::LazyInitializer<SkeletalRegion> skeletalSingleton;

void SkeletalRegion::initialize() {
	skeletalSingleton.initialize();
}

SkeletalRegion &SkeletalRegion::global() {
	return *skeletalSingleton;
}

void *SkeletalRegion::access(PhysicalAddr physical) {
	assert(!(physical & (kPageSize - 1)));
	return reinterpret_cast<void *>(0xFFFF'8000'0000'0000 + physical);
}

// --------------------------------------------------------
// PhysicalChunkAllocator
// --------------------------------------------------------

PhysicalChunkAllocator::PhysicalChunkAllocator() {
}

void PhysicalChunkAllocator::bootstrapRegion(PhysicalAddr address,
		int order, size_t numRoots, int8_t *buddyTree) {
	if(_numRegions >= 8) {
		frigg::infoLogger() << "thor: Ignoring memory region (can only handle 8 regions)"
				<< frigg::endLog;
		return;
	}

	int n = _numRegions++;
	_allRegions[n].physicalBase = address;
	_allRegions[n].regionSize = numRoots << (order + kPageShift);
	_allRegions[n].buddyAccessor = BuddyAccessor{address, kPageShift,
			buddyTree, numRoots, order};

	_freePages += numRoots << order;
}

PhysicalAddr PhysicalChunkAllocator::allocate(size_t size, int addressBits) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	assert(_freePages > size / kPageSize);
	_freePages -= size / kPageSize;
	_usedPages += size / kPageSize;

	// TODO: This could be solved better.
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;
	assert(size == (size_t(kPageSize) << target));

	if(logPhysicalAllocs)
		frigg::infoLogger() << "thor: Allocating physical memory of order "
					<< (target + kPageShift) << frigg::endLog;
	for(int i = 0; i < _numRegions; i++) {
		if(target > _allRegions[i].buddyAccessor.tableOrder())
			continue;

		auto physical = _allRegions[i].buddyAccessor.allocate(target, addressBits);
		if(physical == BuddyAccessor::illegalAddress)
			continue;
	//	frigg::infoLogger() << "Allocate " << (void *)physical << frigg::endLog;
		assert(!(physical % (size_t(kPageSize) << target)));
		return physical;
	}

	return static_cast<PhysicalAddr>(-1);
}

void PhysicalChunkAllocator::free(PhysicalAddr address, size_t size) {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);
	
	int target = 0;
	while(size > (size_t(kPageSize) << target))
		target++;

	for(int i = 0; i < _numRegions; i++) {
		if(address < _allRegions[i].physicalBase)
			continue;
		if(address + size - _allRegions[i].physicalBase > _allRegions[i].regionSize)
			continue;

		_allRegions[i].buddyAccessor.free(address, target);
		assert(_usedPages > size / kPageSize);
		_freePages += size / kPageSize;
		_usedPages -= size / kPageSize;
		return;
	}

	assert(!"Physical page is not part of any region");
}

size_t PhysicalChunkAllocator::numUsedPages() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	return _usedPages;
}
size_t PhysicalChunkAllocator::numFreePages() {
	auto irq_lock = frigg::guard(&irqMutex());
	auto lock = frigg::guard(&_mutex);

	return _freePages;
}

} // namespace thor

