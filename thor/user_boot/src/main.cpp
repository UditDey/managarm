
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <sys/auxv.h>
#include <string>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>
#include <frigg/elf.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include <xuniverse.pb.h>

// TODO: remove/rename those functions from mlibc
HelHandle __raw_map(int fd);
int __mlibc_pushFd(HelHandle handle);

struct ImageInfo {
	ImageInfo()
	: entryIp(nullptr) { }

	void *entryIp;
	void *phdrPtr;
	size_t phdrEntrySize;
	size_t phdrCount;
	std::string interpreter;
};

ImageInfo loadImage(HelHandle space, const char *path, uintptr_t base) {
	ImageInfo info;

	// open and map the executable image into this address space.
	int fd = open(path, O_RDONLY);
	HelHandle image_handle = __raw_map(fd);
	// TODO: close the image file.

	size_t size;
	void *image_ptr;
	HEL_CHECK(helMemoryInfo(image_handle, &size));
	HEL_CHECK(helMapMemory(image_handle, kHelNullHandle, nullptr, 0, size,
			kHelMapReadOnly, &image_ptr));
	HEL_CHECK(helCloseDescriptor(image_handle));
	
	Elf64_Ehdr *ehdr = (Elf64_Ehdr*)image_ptr;
	assert(ehdr->e_ident[0] == 0x7F
			&& ehdr->e_ident[1] == 'E'
			&& ehdr->e_ident[2] == 'L'
			&& ehdr->e_ident[3] == 'F');
	assert(ehdr->e_type == ET_EXEC || ehdr->e_type == ET_DYN);

	info.entryIp = (char *)base + ehdr->e_entry;
	info.phdrEntrySize = ehdr->e_phentsize;
	info.phdrCount = ehdr->e_phnum;
	
	for(int i = 0; i < ehdr->e_phnum; i++) {
		auto phdr = (Elf64_Phdr *)((uintptr_t)image_ptr + ehdr->e_phoff
				+ i * ehdr->e_phentsize);

		if(phdr->p_type == PT_LOAD) {
			const size_t kPageSize = 0x1000;

			// align virtual address and length to page size
			uintptr_t virt_address = base + phdr->p_vaddr;
			virt_address -= virt_address % kPageSize;

			size_t virt_length = (base + phdr->p_vaddr + phdr->p_memsz) - virt_address;
			if((virt_length % kPageSize) != 0)
				virt_length += kPageSize - virt_length % kPageSize;
			
			// map the segment memory as read/write and initialize it
			HelHandle memory;
			HEL_CHECK(helAllocateMemory(virt_length, 0, &memory));
			
			void *write_ptr;
			HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, virt_length,
					kHelMapReadWrite, &write_ptr));

			memset(write_ptr, 0, virt_length);
			memcpy((char *)write_ptr + (base + phdr->p_vaddr - virt_address),
					(char *)image_ptr + phdr->p_offset, phdr->p_filesz);
			HEL_CHECK(helUnmapMemory(kHelNullHandle, write_ptr, virt_length));
			
			// map the segment memory to its own address space
			uint32_t map_flags = 0;
			if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_W)) {
				map_flags |= kHelMapReadWrite;
			}else if((phdr->p_flags & (PF_R | PF_W | PF_X)) == (PF_R | PF_X)) {
				map_flags |= kHelMapReadExecute;
			}else{
				printf("Illegal combination of segment permissions\n");
				abort();
			}

			void *actual_ptr;
			HEL_CHECK(helMapMemory(memory, space, (void *)virt_address, 0, virt_length,
					map_flags, &actual_ptr));
			HEL_CHECK(helCloseDescriptor(memory));
		}else if(phdr->p_type == PT_PHDR) {
			info.phdrPtr = (char *)base + phdr->p_vaddr;
		}else if(phdr->p_type == PT_INTERP) {
			info.interpreter = std::string((char *)image_ptr + phdr->p_offset,
					phdr->p_filesz);
		}else if(phdr->p_type == PT_DYNAMIC
				|| phdr->p_type == PT_GNU_EH_FRAME
				|| phdr->p_type == PT_GNU_STACK) {
			// ignore the phdr
		}else{
			printf("Unexpected PHDR");
			abort();
		}
	}

	return info;
}

COFIBER_ROUTINE(cofiber::no_future, monitorUniverse(HelHandle handle), [=] () {

})

void runProgram(HelHandle space, ImageInfo exec_info, ImageInfo interp_info,
		bool exclusive) {
	constexpr size_t stack_size = 0x10000;

	// TODO: we should use some dup request here to avoid requestId clashes.
	unsigned long fs_server;
	if(peekauxval(AT_FS_SERVER, &fs_server))
		throw std::runtime_error("No AT_FS_SERVER specified");

	HelHandle universe;
	HEL_CHECK(helCreateUniverse(&universe));

	HelHandle remote_fs;
	HEL_CHECK(helTransferDescriptor(fs_server, universe, &remote_fs));

	// setup the auxiliary vector and copy it to the target stack.
	uintptr_t stack_image[] = {
		AT_ENTRY,
		(uintptr_t)exec_info.entryIp,
		AT_PHDR,
		(uintptr_t)exec_info.phdrPtr,
		AT_PHENT,
		exec_info.phdrEntrySize,
		AT_PHNUM,
		exec_info.phdrCount,
		AT_FS_SERVER,
		(uintptr_t)remote_fs,
		AT_NULL,
		0
	};
	
	HelHandle stack_memory;
	HEL_CHECK(helAllocateMemory(stack_size, kHelAllocOnDemand, &stack_memory));
	
	void *write_ptr;
	HEL_CHECK(helMapMemory(stack_memory, kHelNullHandle, nullptr, 0, stack_size,
			kHelMapReadWrite, &write_ptr));
	memcpy((char *)write_ptr + stack_size - sizeof(stack_image),
			&stack_image, sizeof(stack_image));
	HEL_CHECK(helUnmapMemory(kHelNullHandle, write_ptr, stack_size));

	// map the stack into the new address space.
	void *stack_base;
	HEL_CHECK(helMapMemory(stack_memory, space, nullptr,
			0, stack_size, kHelMapReadWrite, &stack_base));
	HEL_CHECK(helCloseDescriptor(stack_memory));

	HelHandle thread;
	uint32_t thread_flags = kHelThreadTrapsAreFatal;
	if(exclusive)
		thread_flags |= kHelThreadExclusive;
	auto directory = helx::Directory::create();
	HEL_CHECK(helCreateThread(universe, space, directory.getHandle(), kHelAbiSystemV,
			interp_info.entryIp, (char *)stack_base + stack_size - sizeof(stack_image),
			thread_flags, &thread));
	HEL_CHECK(helCloseDescriptor(space));

	monitorUniverse(universe);
}

helx::EventHub eventHub = helx::EventHub::create();
helx::Client mbusConnect;
helx::Client acpiConnect;
helx::Pipe posixPipe;

void startMbus() {
	helix::UniquePipe parent_pipe, child_pipe;
	std::tie(parent_pipe, child_pipe) = helix::createFullPipe();
		
	HelHandle space;
	HEL_CHECK(helCreateSpace(&space));

	ImageInfo exec_info = loadImage(space, "mbus", 0);
	// TODO: actually use the correct interpreter
	ImageInfo interp_info = loadImage(space, "ld-init.so", 0x40000000);
	printf("Ready to run\n");
	runProgram(space, exec_info, interp_info, true);
	
	// receive a client handle from the child process
	/*HelError recv_error;
	HelHandle connect_handle;
	parent_pipe.recvDescriptorReqSync(eventHub, kHelAnyRequest, kHelAnySequence,
			recv_error, connect_handle);
	HEL_CHECK(recv_error);
	mbusConnect = helx::Client(connect_handle);*/
}

/*void startAcpi() {
	auto directory = helx::Directory::create();
	HelHandle universe = loadImage("initrd/acpi", directory.getHandle(), true);
	helx::Pipe pipe(universe);
	
	// TODO: use a real protocol here!
	HelError mbus_error;
	pipe.sendDescriptorRespSync(mbusConnect.getHandle(), eventHub, 1001, 0, mbus_error);
	HEL_CHECK(mbus_error);

	// receive a client handle from the child process
	HelError recv_error;
	HelHandle connect_handle;
	pipe.recvDescriptorReqSync(eventHub, kHelAnyRequest, kHelAnySequence,
			recv_error, connect_handle);
	HEL_CHECK(recv_error);
	acpiConnect = helx::Client(connect_handle);
}

void startInitrd() {
	helx::Pipe parent_pipe, child_pipe;
	helx::Pipe::createFullPipe(child_pipe, parent_pipe);

	auto local_directory = helx::Directory::create();
	local_directory.publish(child_pipe.getHandle(), "parent");
	local_directory.publish(mbusConnect.getHandle(), "mbus");
	
	auto directory = helx::Directory::create();
	directory.mount(local_directory.getHandle(), "local");
	directory.remount("initrd/#this", "initrd");
	loadImage("initrd/initrd", directory.getHandle(), true);
	
	// wait until the initrd driver is ready
	HelError error;
	size_t length;
	parent_pipe.recvStringReqSync(nullptr, 0, eventHub, 0, 0, error, length);
	HEL_CHECK(error);
}

void startPosixSubsystem() {
	helx::Pipe parent_pipe, child_pipe;
	helx::Pipe::createFullPipe(child_pipe, parent_pipe);

	auto local_directory = helx::Directory::create();
	local_directory.publish(child_pipe.getHandle(), "parent");
	local_directory.publish(mbusConnect.getHandle(), "mbus");
	
	auto directory = helx::Directory::create();
	directory.mount(local_directory.getHandle(), "local");
	directory.remount("initrd/#this", "initrd");
	loadImage("initrd/posix-subsystem", directory.getHandle(), false);
	
	// receive a client handle from the child process
	HelError recv_error;
	HelHandle connect_handle;
	parent_pipe.recvDescriptorReqSync(eventHub, kHelAnyRequest, kHelAnySequence,
			recv_error, connect_handle);
	HEL_CHECK(recv_error);
	
	helx::Client posix_connect(connect_handle);
	HelError connect_error;
	posix_connect.connectSync(eventHub, connect_error, posixPipe);
	HEL_CHECK(connect_error);
}

void posixDoRequest(managarm::posix::ClientRequest<Allocator> &request,
		managarm::posix::ServerResponse<Allocator> &response, int64_t request_id) {

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	
	HelError send_error; 
	posixPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, request_id, 0, send_error);
	HEL_CHECK(send_error);

	uint8_t buffer[128];
	HelError recv_error;
	size_t length;
	posixPipe.recvStringRespSync(buffer, 128, eventHub, request_id, 0, recv_error, length);
	HEL_CHECK(recv_error);
	response.ParseFromArray(buffer, length);
}

void posixDoRequestWithHandle(HelHandle handle, managarm::posix::ClientRequest<Allocator> &request,
		managarm::posix::ServerResponse<Allocator> &response, int64_t request_id) {

	frigg::String<Allocator> serialized(*allocator);
	request.SerializeToString(&serialized);
	
	HelError send_error; 
	posixPipe.sendStringReqSync(serialized.data(), serialized.size(),
			eventHub, request_id, 0, send_error);
	HEL_CHECK(send_error);

	HelError descriptor_error;
	posixPipe.sendDescriptorReqSync(handle, eventHub, request_id, 1, descriptor_error);
	HEL_CHECK(descriptor_error);
	
	uint8_t buffer[128];
	HelError recv_error;
	size_t length;
	posixPipe.recvStringRespSync(buffer, 128, eventHub, request_id, 0, recv_error, length);
	HEL_CHECK(recv_error);
	response.ParseFromArray(buffer, length);
}

void runPosixInit() {
	// first we send an INIT request to create an initial process
	managarm::posix::ClientRequest<Allocator> init_request(*allocator);
	init_request.set_request_type(managarm::posix::ClientRequestType::INIT);
	
	managarm::posix::ServerResponse<Allocator> init_response(*allocator);
	posixDoRequest(init_request, init_response, 1);
	assert(init_response.error() == managarm::posix::Errors::SUCCESS);
	
	// create a helfd file for the hardware driver
	managarm::posix::ClientRequest<Allocator> open_request(*allocator);
	open_request.set_request_type(managarm::posix::ClientRequestType::OPEN);
	open_request.set_path(frigg::String<Allocator>(*allocator, "/dev/sysfile/hw"));
	open_request.set_flags(managarm::posix::OpenFlags::CREAT);
	open_request.set_mode(managarm::posix::OpenMode::HELFD);
	
	managarm::posix::ServerResponse<Allocator> open_response(*allocator);
	posixDoRequest(open_request, open_response, 2);
	assert(open_response.error() == managarm::posix::Errors::SUCCESS);
	
	// attach the server to the helfd
	managarm::posix::ClientRequest<Allocator> attach_request(*allocator);
	attach_request.set_request_type(managarm::posix::ClientRequestType::HELFD_ATTACH);
	attach_request.set_fd(open_response.fd());

	managarm::posix::ServerResponse<Allocator> attach_response(*allocator);
	posixDoRequestWithHandle(acpiConnect.getHandle(), attach_request, attach_response, 3);
	assert(attach_response.error() == managarm::posix::Errors::SUCCESS);
	
	// after that we EXEC the actual init program
	managarm::posix::ClientRequest<Allocator> exec_request(*allocator);
	exec_request.set_request_type(managarm::posix::ClientRequestType::EXEC);
	exec_request.set_path(frigg::String<Allocator>(*allocator, "/initrd/posix-init"));
	
	managarm::posix::ServerResponse<Allocator> exec_response(*allocator);
	posixDoRequest(exec_request, exec_response, 4);
	assert(exec_response.error() == managarm::posix::Errors::SUCCESS);
}*/

using Dispatcher = helix::Dispatcher<helix::AwaitMechanism>;
Dispatcher dispatcher(helix::createHub());

#include <functional>

helix::UniquePipe server, client;

extern "C" void __rtdl_setupTcb();

COFIBER_ROUTINE(cofiber::no_future, serveStdout(helix::UniquePipe pipe),
		std::bind([=] (helix::UniquePipe pipe) {
	while(true) {
		char req_buffer[128];
		Dispatcher::RecvString recv_req(dispatcher, pipe, req_buffer, 128,
				kHelAnyRequest, 0, kHelRequest);
		COFIBER_AWAIT recv_req.future();

		//FIXME: actually parse the protocol.

		char data_buffer[128];
		Dispatcher::RecvString recv_data(dispatcher, pipe, data_buffer, 128,
				recv_req.requestId(), 1, kHelRequest);
		COFIBER_AWAIT recv_data.future();

		helLog(data_buffer, recv_data.actualLength());

		// send the success response.
		// FIXME: send an actually valid answer.
		Dispatcher::SendString send_resp(dispatcher, pipe, nullptr, 0,
				recv_req.requestId(), 0, kHelResponse);
		COFIBER_AWAIT send_resp.future();
	}
}, std::move(pipe)))

void serveMain() {
	// we use the raw managarm thread API so we have to setup
	// the TCB ourselfs here.
	assert(__rtdl_setupTcb);
	__rtdl_setupTcb();

	serveStdout(std::move(server));

	while(true)
		dispatcher();
	__builtin_trap();
}

int main() {
	// first we start an internal server that gives us an stdout stream.
	HelHandle thread_handle;
	HEL_CHECK(helCreateThread(kHelNullHandle, kHelNullHandle, kHelNullHandle,
			kHelAbiSystemV, (void *)serveMain, (char *)malloc(0x10000) + 0x10000,
			kHelThreadExclusive, &thread_handle));

	std::tie(server, client) = helix::createFullPipe();
	__mlibc_pushFd(client.getHandle());
	__mlibc_pushFd(client.getHandle());
	__mlibc_pushFd(client.getHandle());
//	client.release();

	printf("Entering user_boot\n");
	
	startMbus();
//	startAcpi();
//	startInitrd();

	// hack to synchronize posix subsystem and initrd
	for(int i = 0; i < 10000; i++)
		HEL_CHECK(helYield());

//	startPosixSubsystem();
//	runPosixInit();
	
	printf("user_boot completed successfully\n");
}

