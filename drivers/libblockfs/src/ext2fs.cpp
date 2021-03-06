
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iostream>

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include "ext2fs.hpp"

namespace blockfs {
namespace ext2fs {

namespace {
	constexpr bool logSuperblock = true;

	constexpr int pageShift = 12;
	constexpr size_t pageSize = size_t{1} << pageShift;
}

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

Inode::Inode(FileSystem &fs, uint32_t number)
: fs(fs), number(number), isReady(false) { }

async::result<std::optional<DirEntry>>
Inode::findEntry(std::string name) {
	assert(!name.empty() && name != "." && name != "..");

	co_await readyJump.async_wait();

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{frontalMemory},
			0, map_size,
			kHelMapProtRead | kHelMapDontRequireBacking};

	// Read the directory structure.
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(file_map.get()) + offset);

		if(disk_entry->inode
				&& name.length() == disk_entry->nameLength
				&& !memcmp(disk_entry->name, name.data(), name.length())) {
			DirEntry entry;
			entry.inode = disk_entry->inode;

			switch(disk_entry->fileType) {
			case EXT2_FT_REG_FILE:
				entry.fileType = kTypeRegular; break;
			case EXT2_FT_DIR:
				entry.fileType = kTypeDirectory; break;
			case EXT2_FT_SYMLINK:
				entry.fileType = kTypeSymlink; break;
			default:
				entry.fileType = kTypeNone;
			}

			co_return entry;
		}

		offset += disk_entry->recordLength;
	}
	assert(offset == fileSize());

	co_return std::nullopt;
}

async::result<std::optional<DirEntry>>
Inode::link(std::string name, int64_t ino, blockfs::FileType type) {
	assert(!name.empty() && name != "." && name != "..");
	assert(ino);

	co_await readyJump.async_wait();

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{frontalMemory},
			0, map_size,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	// Space required for the new directory entry.
	auto required = (sizeof(DiskDirEntry) + name.size() + 3) & ~size_t(3);

	// Walk the directory structure.
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto previous_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(file_map.get()) + offset);

		// Calculate available space after we contract previous_entry.
		auto contracted = (sizeof(DiskDirEntry) + previous_entry->nameLength + 3) & ~size_t(3);
		assert(previous_entry->recordLength >= contracted);
		auto available = previous_entry->recordLength - contracted;

		// Check whether we can shrink previous_entry and insert a new entry after it.
		if(available >= required) {
			// Create the new dentry.
			auto disk_entry = reinterpret_cast<DiskDirEntry *>(
					reinterpret_cast<char *>(file_map.get()) + offset + contracted);
			memset(disk_entry, 0, sizeof(DiskDirEntry));
			disk_entry->inode = ino;
			disk_entry->recordLength = available;
			disk_entry->nameLength = name.length();
			switch (type) {
				case kTypeRegular:
					disk_entry->fileType = EXT2_FT_REG_FILE;
					break;
				case kTypeDirectory:
					disk_entry->fileType = EXT2_FT_DIR;
					break;
				default:
					throw std::runtime_error("unexpected type");
			}
			memcpy(disk_entry->name, name.data(), name.length());

			// Update the existing dentry.
			previous_entry->recordLength = contracted;

			// Update the inode.
			auto target = fs.accessInode(ino);
			co_await target->readyJump.async_wait();
			target->diskInode()->linksCount++;

			// Hack: For now, we just remap the inode to make sure the dirty bit is checked.
			auto inode_address = (target->number - 1) * fs.inodeSize;
			target->diskMapping = helix::Mapping{fs.inodeTable,
					inode_address, fs.inodeSize,
					kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

			DirEntry entry;
			entry.inode = ino;
			entry.fileType = type;
			co_return entry;
		}

		offset += previous_entry->recordLength;
	}
	assert(offset == fileSize());

	throw std::runtime_error("Not enough space for ext2fs directory entry");
}

async::result<void> Inode::unlink(std::string name) {
	assert(!name.empty() && name != "." && name != "..");

	co_await readyJump.async_wait();

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{frontalMemory},
			0, map_size,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	// Read the directory structure.
	DiskDirEntry *previous_entry = nullptr;
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(file_map.get()) + offset);

		if(disk_entry->inode
				&& name.length() == disk_entry->nameLength
				&& !memcmp(disk_entry->name, name.data(), name.length())) {
			// The directory should start with "." and "..". As those entries are never deleted,
			// we can assume that a previous entry exists.
			assert(previous_entry);
			previous_entry->recordLength += disk_entry->recordLength;
			co_return;
		}

		offset += disk_entry->recordLength;
		previous_entry = disk_entry;
	}
	assert(offset == fileSize());

	throw std::runtime_error("Given link does not exist");
}

async::result<std::optional<DirEntry>> Inode::mkdir(std::string name) {
	assert(!name.empty() && name != "." && name != "..");

	co_await readyJump.async_wait();

	auto dir_node = co_await fs.createDirectory();
	co_await dir_node->readyJump.async_wait();

	co_await fs.assignDataBlocks(dir_node.get(), 0, 1);

	helix::LockMemoryView lock_memory;
	auto map_size = (dir_node->fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(dir_node->frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{dir_node->frontalMemory},
			0, map_size,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	// XXX: this is a hack to make the directory accessible under
	// OSes that respect the permissions, this means "drwxr-xr-x"
	dir_node->diskInode()->mode = 0x41ED;

	size_t offset = 0;

	auto dot_entry = reinterpret_cast<DiskDirEntry *>(file_map.get());
	offset += (sizeof(DiskDirEntry) + 1 + 3) & ~size_t(3);

	dot_entry->inode = dir_node->number;
	dot_entry->recordLength = offset;
	dot_entry->nameLength = 1;
	dot_entry->fileType = EXT2_FT_DIR;
	memcpy(dot_entry->name, ".", 1);

	auto dot_dot_entry = reinterpret_cast<DiskDirEntry *>(
			reinterpret_cast<char *>(file_map.get()) + offset);

	dot_dot_entry->inode = number;
	dot_dot_entry->recordLength = dir_node->fileSize() - offset;
	dot_dot_entry->nameLength = 2;
	dot_dot_entry->fileType = EXT2_FT_DIR;
	memcpy(dot_dot_entry->name, "..", 2);

	auto inode_address = (dir_node->number - 1) * fs.inodeSize;
	dir_node->diskMapping = helix::Mapping{fs.inodeTable,
			inode_address, fs.inodeSize,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	co_return co_await link(name, dir_node->number, kTypeDirectory);
}

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

FileSystem::FileSystem(BlockDevice *device)
: device(device) {
}

async::result<void> FileSystem::init() {
	std::vector<uint8_t> buffer(1024);
	co_await device->readSectors(2, buffer.data(), 2);

	DiskSuperblock sb;
	memcpy(&sb, buffer.data(), sizeof(DiskSuperblock));
	assert(sb.magic == 0xEF53);

	inodeSize = sb.inodeSize;
	blockShift = 10 + sb.logBlockSize;
	blockSize = 1024 << sb.logBlockSize;
	blockPagesShift = blockShift < pageShift ? pageShift : blockShift;
	sectorsPerBlock = blockSize / 512;
	blocksPerGroup = sb.blocksPerGroup;
	inodesPerGroup = sb.inodesPerGroup;
	numBlockGroups = (sb.blocksCount + (sb.blocksPerGroup - 1)) / sb.blocksPerGroup;

	if(logSuperblock) {
		std::cout << "ext2fs: Revision is: " << sb.revLevel << std::endl;
		std::cout << "ext2fs: Block size is: " << blockSize << std::endl;
		std::cout << "ext2fs:     There are " << sb.blocksCount << " blocks" << std::endl;
		std::cout << "ext2fs: Inode size is: " << inodeSize << std::endl;
		std::cout << "ext2fs:     There are " << sb.inodesCount << " blocks" << std::endl;
		std::cout << "ext2fs:     First available inode is: " << sb.firstIno << std::endl;
		std::cout << "ext2fs: Optional features: " << sb.featureCompat
				<< ", w-required features: " << sb.featureRoCompat
				<< ", r/w-required features: " << sb.featureIncompat << std::endl;
		std::cout << "ext2fs: There are " << numBlockGroups << " block groups" << std::endl;
		std::cout << "ext2fs:     Blocks per group: " << blocksPerGroup << std::endl;
		std::cout << "ext2fs:     Inodes per group: " << inodesPerGroup << std::endl;
	}

	auto bgdt_size = (numBlockGroups * sizeof(DiskGroupDesc) + 511) & ~size_t(511);
	// TODO: Use std::string instead of malloc().
	blockGroupDescriptorBuffer = malloc(bgdt_size);

	auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
	co_await device->readSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
			blockGroupDescriptorBuffer, bgdt_size / 512);

	// Create memory bundles to manage the block and inode bitmaps.
	HelHandle block_bitmap_frontal, inode_bitmap_frontal;
	HelHandle block_bitmap_backing, inode_bitmap_backing;
	HEL_CHECK(helCreateManagedMemory(numBlockGroups << blockPagesShift,
			kHelAllocBacked, &block_bitmap_backing, &block_bitmap_frontal));
	HEL_CHECK(helCreateManagedMemory(numBlockGroups << blockPagesShift,
			kHelAllocBacked, &inode_bitmap_backing, &inode_bitmap_frontal));
	blockBitmap = helix::UniqueDescriptor{block_bitmap_frontal};
	inodeBitmap = helix::UniqueDescriptor{inode_bitmap_frontal};

	manageBlockBitmap(helix::UniqueDescriptor{block_bitmap_backing});
	manageInodeBitmap(helix::UniqueDescriptor{inode_bitmap_backing});

	// Create a memory bundle to manage the inode table.
	assert(!((inodesPerGroup * inodeSize) & 0xFFF));
	HelHandle inode_table_frontal;
	HelHandle inode_table_backing;
	HEL_CHECK(helCreateManagedMemory(inodesPerGroup * inodeSize * numBlockGroups,
			kHelAllocBacked, &inode_table_backing, &inode_table_frontal));
	inodeTable = helix::UniqueDescriptor{inode_table_frontal};

	manageInodeTable(helix::UniqueDescriptor{inode_table_backing});

	co_return;
}

async::detached FileSystem::manageBlockBitmap(helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		auto bg_idx = manage.offset() >> blockPagesShift;
		auto bgdt = (DiskGroupDesc *)blockGroupDescriptorBuffer;
		auto block = bgdt[bg_idx].blockBitmap;
		assert(block);

		assert(!(manage.offset() & ((1 << blockPagesShift) - 1))
				&& "TODO: propery support multi-page blocks");
		assert(manage.length() == (1 << blockPagesShift)
				&& "TODO: propery support multi-page blocks");

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping bitmap_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->readSectors(block * sectorsPerBlock,
					bitmap_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		}else{
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping bitmap_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->writeSectors(block * sectorsPerBlock,
					bitmap_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

async::detached FileSystem::manageInodeBitmap(helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		auto bg_idx = manage.offset() >> blockPagesShift;
		auto bgdt = (DiskGroupDesc *)blockGroupDescriptorBuffer;
		auto block = bgdt[bg_idx].inodeBitmap;
		assert(block);

		assert(!(manage.offset() & ((1 << blockPagesShift) - 1))
				&& "TODO: propery support multi-page blocks");
		assert(manage.length() == (1 << blockPagesShift)
				&& "TODO: propery support multi-page blocks");

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping bitmap_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->readSectors(block * sectorsPerBlock,
					bitmap_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		}else{
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping bitmap_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->writeSectors(block * sectorsPerBlock,
					bitmap_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

async::detached FileSystem::manageInodeTable(helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		// TODO: Make sure that we do not read/write past the end of the table.
		assert(!((inodesPerGroup * inodeSize) & (blockSize - 1)));

		// TODO: Use shifts instead of division.
		auto bg_idx = manage.offset() / (inodesPerGroup * inodeSize);
		auto bg_offset = manage.offset() % (inodesPerGroup * inodeSize);
		auto bgdt = (DiskGroupDesc *)blockGroupDescriptorBuffer;
		auto block = bgdt[bg_idx].inodeTable;
		assert(block);

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping table_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->readSectors(block * sectorsPerBlock + bg_offset / 512,
					table_map.get(), manage.length() / 512);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		}else{
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping table_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->writeSectors(block * sectorsPerBlock + bg_offset / 512,
					table_map.get(), manage.length() / 512);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

auto FileSystem::accessRoot() -> std::shared_ptr<Inode> {
	return accessInode(EXT2_ROOT_INO);
}

auto FileSystem::accessInode(uint32_t number) -> std::shared_ptr<Inode> {
	assert(number > 0);
	std::weak_ptr<Inode> &inode_slot = activeInodes[number];
	std::shared_ptr<Inode> active_inode = inode_slot.lock();
	if(active_inode)
		return active_inode;

	auto new_inode = std::make_shared<Inode>(*this, number);
	inode_slot = std::weak_ptr<Inode>(new_inode);
	initiateInode(new_inode);

	return new_inode;
}

async::result<std::shared_ptr<Inode>> FileSystem::createRegular() {
	auto ino = co_await allocateInode();
	assert(ino);

	// Lock and map the inode table.
	auto inode_address = (ino - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inode_address & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());

	helix::Mapping inode_map{inodeTable,
				inode_address, inodeSize,
				kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(inode_map.get());
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFREG;
	disk_inode->generation = generation + 1;

	co_return accessInode(ino);
}

async::result<std::shared_ptr<Inode>> FileSystem::createDirectory() {
	auto ino = co_await allocateInode();
	assert(ino);

	// Lock and map the inode table.
	auto inode_address = (ino - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inode_address & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());

	helix::Mapping inode_map{inodeTable,
				inode_address, inodeSize,
				kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(inode_map.get());
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFDIR;
	disk_inode->generation = generation + 1;
	disk_inode->size = blockSize;

	co_return accessInode(ino);
}

async::result<void> FileSystem::write(Inode *inode, uint64_t offset,
		const void *buffer, size_t length) {
	co_await inode->readyJump.async_wait();

	// Make sure that data blocks are allocated.
	auto block_offset = (offset & ~(blockSize - 1)) >> blockShift;
	auto block_count = ((offset & (blockSize - 1)) + length + (blockSize - 1)) >> blockShift;
	co_await assignDataBlocks(inode, block_offset, block_count);

	// Resize the file if necessary.
	if(offset + length > inode->fileSize()) {
		HEL_CHECK(helResizeMemory(inode->backingMemory,
				(offset + length + 0xFFF) & ~size_t(0xFFF)));
		inode->setFileSize(offset + length);

		// Notify the kernel that the inode might have changed.
		// Hack: For now, we just remap the inode to make sure the dirty bit is checked.
		auto inode_address = (inode->number - 1) * inodeSize;

		inode->diskMapping = helix::Mapping{inodeTable,
				inode_address, inodeSize,
				kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};
	}

	auto map_offset = offset & ~size_t(0xFFF);
	auto map_size = ((offset & size_t(0xFFF)) + length + 0xFFF) & ~size_t(0xFFF);

	helix::LockMemoryView lock_memory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(inode->frontalMemory),
			&lock_memory, map_offset, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{inode->frontalMemory},
			static_cast<ptrdiff_t>(map_offset), map_size,
			kHelMapProtWrite | kHelMapDontRequireBacking};

	memcpy(reinterpret_cast<char *>(file_map.get()) + (offset - map_offset),
			buffer, length);
}

async::detached FileSystem::initiateInode(std::shared_ptr<Inode> inode) {
	// TODO: Use a shift instead of a division.
	auto inode_address = (inode->number - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inode_address & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());
	inode->diskLock = lock_inode.descriptor();

	inode->diskMapping = helix::Mapping{inodeTable,
			inode_address, inodeSize,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};
	auto disk_inode = inode->diskInode();
//	printf("Inode %u: file size: %u\n", inode->number, disk_inode.size);

	if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
		inode->fileType = kTypeRegular;
	}else if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
		inode->fileType = kTypeSymlink;
	}else if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
		inode->fileType = kTypeDirectory;
	}else{
		std::cerr << "ext2fs: Unexpected inode type " << (disk_inode->mode & EXT2_S_IFMT)
				<< " for inode " << inode->number << std::endl;
		abort();
	}

	// TODO: support large files
	inode->fileData = disk_inode->data;

	// filter out the file type from the mode
	// TODO: ext2fs stores a 32-bit mode
	inode->mode = disk_inode->mode & 0x0FFF;

	inode->numLinks = disk_inode->linksCount;
	// TODO: support large uid / gids
	inode->uid = disk_inode->uid;
	inode->gid = disk_inode->gid;
	inode->accessTime.tv_sec = disk_inode->atime;
	inode->accessTime.tv_nsec = 0;
	inode->dataModifyTime.tv_sec = disk_inode->mtime;
	inode->dataModifyTime.tv_nsec = 0;
	inode->anyChangeTime.tv_sec = disk_inode->ctime;
	inode->anyChangeTime.tv_nsec = 0;

	// Allocate a page cache for the file.
	auto cache_size = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);
	HEL_CHECK(helCreateManagedMemory(cache_size, kHelAllocBacked,
			&inode->backingMemory, &inode->frontalMemory));

	inode->isReady = true;
	inode->readyJump.trigger();

	HelHandle frontalOrder1, frontalOrder2;
	HelHandle backingOrder1, backingOrder2;
	HEL_CHECK(helCreateManagedMemory(3 << blockPagesShift,
			kHelAllocBacked, &backingOrder1, &frontalOrder1));
	HEL_CHECK(helCreateManagedMemory((blockSize / 4) << blockPagesShift,
			kHelAllocBacked, &backingOrder2, &frontalOrder2));
	inode->indirectOrder1 = helix::UniqueDescriptor{frontalOrder1};
	inode->indirectOrder2 = helix::UniqueDescriptor{frontalOrder2};

	manageIndirect(inode, 1, helix::UniqueDescriptor{backingOrder1});
	manageIndirect(inode, 2, helix::UniqueDescriptor{backingOrder2});
	manageFileData(inode);
}

async::detached FileSystem::manageFileData(std::shared_ptr<Inode> inode) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(helix::BorrowedDescriptor(inode->backingMemory),
				&manage, helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(manage.error());
		assert(manage.offset() + manage.length() <= ((inode->fileSize() + 0xFFF) & ~size_t(0xFFF)));

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping file_map{helix::BorrowedDescriptor{inode->backingMemory},
					static_cast<ptrdiff_t>(manage.offset()), manage.length(), kHelMapProtWrite};

			assert(!(manage.offset() % inode->fs.blockSize));
			size_t backed_size = std::min(manage.length(), inode->fileSize() - manage.offset());
			size_t num_blocks = (backed_size + (inode->fs.blockSize - 1)) / inode->fs.blockSize;

			assert(num_blocks * inode->fs.blockSize <= manage.length());
			co_await inode->fs.readDataBlocks(inode, manage.offset() / inode->fs.blockSize,
					num_blocks, file_map.get());

			HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageInitialize,
					manage.offset(), manage.length()));
		}else{
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping file_map{helix::BorrowedDescriptor{inode->backingMemory},
					static_cast<ptrdiff_t>(manage.offset()), manage.length(), kHelMapProtRead};

			assert(!(manage.offset() % inode->fs.blockSize));
			size_t backed_size = std::min(manage.length(), inode->fileSize() - manage.offset());
			size_t num_blocks = (backed_size + (inode->fs.blockSize - 1)) / inode->fs.blockSize;

			assert(num_blocks * inode->fs.blockSize <= manage.length());
			co_await inode->fs.writeDataBlocks(inode, manage.offset() / inode->fs.blockSize,
					num_blocks, file_map.get());

			HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

async::detached FileSystem::manageIndirect(std::shared_ptr<Inode> inode,
		int order, helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		uint32_t element = manage.offset() >> blockPagesShift;

		uint32_t block;
		if(order == 1) {
			auto disk_inode = inode->diskInode();

			switch(element) {
			case 0: block = disk_inode->data.blocks.singleIndirect; break;
			case 1: block = disk_inode->data.blocks.doubleIndirect; break;
			case 2: block = disk_inode->data.blocks.tripleIndirect; break;
			default:
				assert(!"unexpected offset");
				abort();
			}
		}else{
			assert(order == 2);

			auto indirect_frame = element >> (blockShift - 2);
			auto indirect_index = element & ((1 << (blockShift - 2)) - 1);

			helix::LockMemoryView lock_indirect;
			auto &&submit_indirect = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_indirect,
					(1 + indirect_frame) << blockPagesShift, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit_indirect.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					(1 + indirect_frame) << blockPagesShift, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};
			block = reinterpret_cast<uint32_t *>(indirect_map.get())[indirect_index];
		}

		assert(!(manage.offset() & ((1 << blockPagesShift) - 1))
				&& "TODO: propery support multi-page blocks");
		assert(manage.length() == (1 << blockPagesShift)
				&& "TODO: propery support multi-page blocks");

		if (manage.type() == kHelManageInitialize) {
			helix::Mapping out_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->readSectors(block * sectorsPerBlock,
					out_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		} else {
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping out_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->writeSectors(block * sectorsPerBlock,
					out_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));

		}
	}
}

async::result<uint32_t> FileSystem::allocateBlock() {
	int64_t bg_idx = 0;

	helix::LockMemoryView lock_bitmap;
	auto &&submit_bitmap = helix::submitLockMemoryView(blockBitmap,
			&lock_bitmap,
			bg_idx << blockPagesShift, 1 << blockPagesShift,
			helix::Dispatcher::global());
	co_await submit_bitmap.async_wait();
	HEL_CHECK(lock_bitmap.error());

	helix::Mapping bitmap_map{blockBitmap,
			bg_idx << blockPagesShift, size_t{1} << blockPagesShift,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	// TODO: Update the block group descriptor table.

	auto words = reinterpret_cast<uint32_t *>(bitmap_map.get());
	for(int i = 0; i < (blocksPerGroup + 31) / 32; i++) {
		if(words[i] == 0xFFFFFFFF)
			continue;
		for(int j = 0; j < 32; j++) {
			if(words[i] & (static_cast<uint32_t>(1) << j))
				continue;
			// TODO: Make sure we never return reserved blocks.
			// TODO: Make sure we never return blocks higher than the max. block in the SB.
			auto block = bg_idx * blocksPerGroup + i * 32 + j;
			assert(block != 0);
			assert(block < blocksPerGroup);
			words[i] |= static_cast<uint32_t>(1) << j;
			co_return block;
		}
		assert(!"Failed to find zero-bit");
	}

	co_return 0;
}

async::result<uint32_t> FileSystem::allocateInode() {
	// TODO: Do not start at block group zero.
	for(uint32_t bg_idx = 0; bg_idx < numBlockGroups; bg_idx++) {
		helix::LockMemoryView lock_bitmap;
		auto &&submit_bitmap = helix::submitLockMemoryView(inodeBitmap,
				&lock_bitmap,
				bg_idx << blockPagesShift, 1 << blockPagesShift,
				helix::Dispatcher::global());
		co_await submit_bitmap.async_wait();
		HEL_CHECK(lock_bitmap.error());

		helix::Mapping bitmap_map{inodeBitmap,
				bg_idx << blockPagesShift, size_t{1} << blockPagesShift,
				kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

		// TODO: Update the block group descriptor table.

		auto words = reinterpret_cast<uint32_t *>(bitmap_map.get());
		for(int i = 0; i < (inodesPerGroup + 31) / 32; i++) {
			if(words[i] == 0xFFFFFFFF)
				continue;
			for(int j = 0; j < 32; j++) {
				if(words[i] & (static_cast<uint32_t>(1) << j))
					continue;
				// TODO: Make sure we never return reserved inodes.
				// TODO: Make sure we never return inodes higher than the max. inode in the SB.
				auto ino = bg_idx * inodesPerGroup + i * 32 + j + 1;
				assert(ino != 0);
				assert(ino < inodesPerGroup);
				words[i] |= static_cast<uint32_t>(1) << j;
				co_return ino;
			}
			assert(!"Failed to find zero-bit");
		}
	}

	co_return 0;
}

async::result<void> FileSystem::assignDataBlocks(Inode *inode,
		uint64_t block_offset, size_t num_blocks) {
	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	auto disk_inode = inode->diskInode();

	size_t prg = 0;
	while(prg < num_blocks) {
		if(block_offset + prg < i_range) {
			while(prg < num_blocks
					&& block_offset + prg < i_range) {
				auto idx = block_offset + prg;
				if(disk_inode->data.blocks.direct[idx]) {
					prg++;
					continue;
				}
				auto block = co_await allocateBlock();
				assert(block && "Out of disk space"); // TODO: Fix this.
				disk_inode->data.blocks.direct[idx] = block;
				prg++;
			}
		}else if(block_offset + prg < s_range) {
			bool needsReset = false;

			// Allocate the single-indirect block itself.
			if(!disk_inode->data.blocks.singleIndirect) {
				auto block = co_await allocateBlock();
				assert(block && "Out of disk space"); // TODO: Fix this.
				disk_inode->data.blocks.singleIndirect = block;
				needsReset = true;
			}

			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_indirect, 0, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					0, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};
			auto window = reinterpret_cast<uint32_t *>(indirect_map.get());

			if(needsReset)
				memset(window, 0, size_t{1} << blockPagesShift);

			while(prg < num_blocks
					&& block_offset + prg < s_range) {
				auto idx = block_offset + prg - i_range;
				if(window[idx]) {
					prg++;
					continue;
				}
				auto block = co_await allocateBlock();
				assert(block && "Out of disk space"); // TODO: Fix this.
				window[idx] = block;
				prg++;
			}
		}else if(block_offset + prg < d_range) {
			assert(!"TODO: Implement allocation in double indirect blocks");
		}else{
			assert(!"TODO: Implement allocation in triple indirect blocks");
		}
	}

	// Notify the kernel that the inode might have changed.
	// Hack: For now, we just remap the inode to make sure the dirty bit is checked.
	auto inode_address = (inode->number - 1) * inodeSize;

	inode->diskMapping = helix::Mapping{inodeTable,
			inode_address, inodeSize,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};
}

async::result<void> FileSystem::readDataBlocks(std::shared_ptr<Inode> inode,
		uint64_t offset, size_t num_blocks, void *buffer) {
	// We perform "block-fusion" here i.e. we try to read/write multiple
	// consecutive blocks in a single read/writeSectors() operation.
	auto fuse = [] (size_t index, size_t remaining, uint32_t *list, size_t limit) {
		size_t n = 1;
		while(n < remaining && index + n < limit) {
			if(list[index + n] != list[index] + n)
				break;
			n++;
		}
		return std::pair<size_t, size_t>{list[index], n};
	};

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	co_await inode->readyJump.async_wait();
	// TODO: Assert that we do not read past the EOF.

	size_t progress = 0;
	while(progress < num_blocks) {
		// Block number and block count of the readSectors() command that we will issue here.
		std::pair<size_t, size_t> issue;

		auto index = offset + progress;
//		std::cout << "Reading " << index << "-th block from inode " << inode->number
//				<< " (" << progress << "/" << num_blocks << " in request)" << std::endl;

		assert(index < d_range);
		if(index >= d_range) {
			assert(!"Fix triple indirect blocks");
		}else if(index >= s_range) { // Use the double indirect block.
			// TODO: Use shift/and instead of div/mod.
			int64_t indirect_frame = (index - s_range) >> (blockShift - 2);
			int64_t indirect_index = (index - s_range) & ((1 << (blockShift - 2)) - 1);

			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder2, &lock_indirect,
					indirect_frame << blockPagesShift, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder2,
					indirect_frame << blockPagesShift, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};

			issue = fuse(indirect_index, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else if(index >= i_range) { // Use the single indirect block.
			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_indirect, 0, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					0, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};
			issue = fuse(index - i_range, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else{
			auto disk_inode = inode->diskInode();

			issue = fuse(index, num_blocks - progress,
					disk_inode->data.blocks.direct, 12);
		}

//		std::cout << "Issuing read of " << issue.second
//				<< " blocks, starting at " << issue.first << std::endl;

		assert(issue.first);
		co_await device->readSectors(issue.first * sectorsPerBlock,
				(uint8_t *)buffer + progress * blockSize,
				issue.second * sectorsPerBlock);
		progress += issue.second;
	}
}

// TODO: There is a lot of overlap between this method and readDataBlocks.
//       Refactor common code into a another method.
async::result<void> FileSystem::writeDataBlocks(std::shared_ptr<Inode> inode,
		uint64_t offset, size_t num_blocks, const void *buffer) {
	// We perform "block-fusion" here i.e. we try to read/write multiple
	// consecutive blocks in a single read/writeSectors() operation.
	auto fuse = [] (size_t index, size_t remaining, uint32_t *list, size_t limit) {
		size_t n = 1;
		while(n < remaining && index + n < limit) {
			if(list[index + n] != list[index] + n)
				break;
			n++;
		}
		return std::pair<size_t, size_t>{list[index], n};
	};

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	co_await inode->readyJump.async_wait();
	// TODO: Assert that we do not write past the EOF.

	size_t progress = 0;
	while(progress < num_blocks) {
		// Block number and block count of the writeSectors() command that we will issue here.
		std::pair<size_t, size_t> issue;

		auto index = offset + progress;
//		std::cout << "Write " << index << "-th block to inode " << inode->number
//				<< " (" << progress << "/" << num_blocks << " in request)" << std::endl;

		assert(index < d_range);
		if(index >= d_range) {
			assert(!"Fix triple indirect blocks");
		}else if(index >= s_range) { // Use the double indirect block.
			// TODO: Use shift/and instead of div/mod.
			int64_t indirect_frame = (index - s_range) >> (blockShift - 2);
			int64_t indirect_index = (index - s_range) & ((1 << (blockShift - 2)) - 1);

			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder2, &lock_indirect,
					indirect_frame << blockPagesShift, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder2,
					indirect_frame << blockPagesShift, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};

			issue = fuse(indirect_index, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else if(index >= i_range) { // Use the triple indirect block.
			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_indirect, 0, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					0, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};
			issue = fuse(index - i_range, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else{
			auto disk_inode = inode->diskInode();

			issue = fuse(index, num_blocks - progress,
					disk_inode->data.blocks.direct, 12);
		}

//		std::cout << "Issuing write of " << issue.second
//				<< " blocks, starting at " << issue.first << std::endl;

		assert(issue.first);
		co_await device->writeSectors(issue.first * sectorsPerBlock,
				(const uint8_t *)buffer + progress * blockSize,
				issue.second * sectorsPerBlock);
		progress += issue.second;
	}
}


async::result<void> FileSystem::truncate(Inode *inode, size_t size) {
	HEL_CHECK(helResizeMemory(inode->backingMemory,
			(size + 0xFFF) & ~size_t(0xFFF)));
	inode->setFileSize(size);

	// Notify the kernel that the inode might have changed.
	// Hack: For now, we just remap the inode to make sure the dirty bit is checked.
	auto inode_address = (inode->number - 1) * inodeSize;

	inode->diskMapping = helix::Mapping{inodeTable,
			inode_address, inodeSize,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};
	co_return;
}

// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

OpenFile::OpenFile(std::shared_ptr<Inode> inode)
: inode(inode), offset(0) { }

async::result<std::optional<std::string>>
OpenFile::readEntries() {
	co_await inode->readyJump.async_wait();

	if (inode->fileType != kTypeDirectory) {
		std::cout << "\e[33m" "ext2fs: readEntries called on something that's not a directory\e[39m" << std::endl;
		co_return std::nullopt; // FIXME: this does not indicate an error
	}

	auto map_size = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);

	helix::LockMemoryView lock_memory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(inode->frontalMemory),
			&lock_memory, 0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{inode->frontalMemory},
			0, map_size,
			kHelMapProtRead | kHelMapDontRequireBacking};

	// Read the directory structure.
	assert(offset <= inode->fileSize());
	while(offset < inode->fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= inode->fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(file_map.get()) + offset);
		assert(offset + disk_entry->recordLength <= inode->fileSize());

		offset += disk_entry->recordLength;

		if(disk_entry->inode) {
		//	std::cout << "libblockfs: Returning entry "
		//			<< std::string(disk_entry->name, disk_entry->nameLength) << std::endl;
			co_return std::string(disk_entry->name, disk_entry->nameLength);
		}
	}
	assert(offset == inode->fileSize());

	co_return std::nullopt;
}

} } // namespace blockfs::ext2fs

