
#include <stdio.h>
#include <stdlib.h>

#include <libchain/all.hpp>

#include <fs.pb.h>
#include <libnet.hpp>

#include "udp.hpp"
#include "tcp.hpp"
#include "dns.hpp"
#include "arp.hpp"
#include "usernet.hpp"
#include "ethernet.hpp"
#include "network.hpp"

template<typename... Args>
frigg::CallbackPtr<void(Args...)> libchainToFrigg(libchain::Callback<void(Args...)> callback) {
	return CALLBACK_STATIC(callback.implementation(),
			&libchain::Callback<void(Args...)>::invoke);
}

namespace libnet {

Network::Network(NetDevice &device)
: device(device) { }

// --------------------------------------------------------
// Client
// --------------------------------------------------------

Client::Client(helx::EventHub &event_hub, Network &net)
: eventHub(event_hub), net(net), objectHandler(*this), mbusConnection(eventHub) {
	mbusConnection.setObjectHandler(&objectHandler);
}

void Client::init(frigg::CallbackPtr<void()> callback) {
	auto closure = new InitClosure(*this, callback);
	(*closure)();
}

// --------------------------------------------------------
// Client::ObjectHandler
// --------------------------------------------------------

Client::ObjectHandler::ObjectHandler(Client &client)
: client(client) { }

void Client::ObjectHandler::requireIf(bragi_mbus::ObjectId object_id,
		frigg::CallbackPtr<void(HelHandle)> callback) {
	helx::Pipe local, remote;
	helx::Pipe::createFullPipe(local, remote);
	callback(remote.getHandle());
	remote.reset();

	auto closure = new Connection(client.eventHub, client.net, std::move(local));
	(*closure)();
}

// --------------------------------------------------------
// Client::InitClosure
// --------------------------------------------------------

Client::InitClosure::InitClosure(Client &client, frigg::CallbackPtr<void()> callback)
: client(client), callback(callback) { }

void Client::InitClosure::operator() () {
	client.mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void Client::InitClosure::connected() {
	client.mbusConnection.registerObject("network",
			CALLBACK_MEMBER(this, &InitClosure::registered));
}

void Client::InitClosure::registered(bragi_mbus::ObjectId object_id) {
	callback();
}

// --------------------------------------------------------
// Connection
// --------------------------------------------------------

Connection::Connection(helx::EventHub &event_hub, Network &net, helx::Pipe pipe)
: eventHub(event_hub), net(net), pipe(std::move(pipe)), nextHandle(1) { }

void Connection::operator() () {
	HEL_CHECK(pipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &Connection::recvRequest)));
}

Network &Connection::getNet() {
	return net;
}

int Connection::attachOpenFile(OpenFile *file) {
	int handle = nextHandle++;
	fileHandles.insert(std::make_pair(handle, file));
	return handle;
}

OpenFile *Connection::getOpenFile(int handle) {
	return fileHandles.at(handle);
}

void Connection::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::CntRequest request;
	request.ParseFromArray(buffer, length);
	
	if(request.req_type() == managarm::fs::CntReqType::OPEN) {
		auto action = libchain::compose([this, request, msg_request] () {
			managarm::fs::SvrResponse response;

			if(request.path() == "ip+udp") {
				int handle = attachOpenFile(new OpenFile);
				response.set_error(managarm::fs::Errors::SUCCESS);
				response.set_file_type(managarm::fs::FileType::SOCKET);
				response.set_fd(handle);
			}else{
				response.set_error(managarm::fs::Errors::FILE_NOT_FOUND);
			}
			
			std::string serialized;
			response.SerializeToString(&serialized);
			
			printf("[libnet/src/network.cpp] recvRequest:OPEN sendStringResp \n");
			return pipe.sendStringResp(serialized.data(), serialized.size(),
					eventHub, msg_request, 0)
			+ libchain::lift([=] (HelError error) { HEL_CHECK(error); });
		});
	
		libchain::run(action);
	}else if(request.req_type() == managarm::fs::CntReqType::CONNECT) {
		auto action = libchain::compose([this, request, msg_request] () {
			managarm::fs::SvrResponse response;
			
			auto file = getOpenFile(request.fd());
			file->address = Ip4Address(192, 168, 178, 43);
			file->port = 1234;
			response.set_error(managarm::fs::Errors::SUCCESS);

			std::string serialized;
			response.SerializeToString(&serialized);
			
			printf("[libnet/src/network.cpp] recvRequest:CONNECT sendStringResp \n");
			return pipe.sendStringResp(serialized.data(), serialized.size(),
					eventHub, msg_request, 0)
			+ libchain::lift([=] (HelError error) { HEL_CHECK(error); });
		});

		libchain::run(action);
	}else if(request.req_type() == managarm::fs::CntReqType::WRITE) {
		auto action = libchain::compose([this, request, msg_request] (std::string *buffer) {
			buffer->resize(request.size());

			return libchain::await<void(HelError, int64_t, int64_t, size_t)>([this, request, msg_request, buffer] (auto callback) {
				HEL_CHECK(pipe.recvStringReq(&(*buffer)[0], request.size(), eventHub, msg_request, 1,
						libchainToFrigg(callback)));
			})
			+ libchain::compose([this, request, msg_request, buffer] (HelError error,
					int64_t msg_request, int64_t msg_seq, size_t length) {
				HEL_CHECK(error);
				assert(length == (size_t)request.size());

				managarm::fs::SvrResponse response;
			
				auto file = getOpenFile(request.fd());
				
				EthernetInfo etherInfo;
				etherInfo.sourceMac = localMac;
				etherInfo.destMac = routerMac;
				etherInfo.etherType = kEtherIp4;
				
				Ip4Info ipInfo;
				ipInfo.sourceIp = localIp;
				ipInfo.destIp = file->address;
				ipInfo.protocol = kUdpProtocol;

				UdpInfo udpInfo;
				udpInfo.sourcePort = 1234;
				udpInfo.destPort = file->port;
				
				sendUdpPacket(net.device, etherInfo, ipInfo, udpInfo, *buffer);
				response.set_error(managarm::fs::Errors::SUCCESS);

				std::string serialized;
				response.SerializeToString(&serialized);
			
				printf("[libnet/src/network.cpp] recvRequest:WRITE sendStringResp \n");
				return pipe.sendStringResp(serialized.data(), serialized.size(),
						eventHub, msg_request, 0)
				+ libchain::lift([=] (HelError error) { HEL_CHECK(error); });
			});
		}, std::string());

		libchain::run(action);
	}/*else if(request.req_type() == managarm::fs::CntReqType::READ) {
		auto closure = new ReadClosure(*this, msg_request, std::move(request));
		(*closure)();
	}*/else{
		fprintf(stderr, "Illegal request type\n");
		abort();
	}

	(*this)();
}

} // namespace libnet

