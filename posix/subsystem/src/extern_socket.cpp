#include "extern_socket.hpp"

#include "fs.pb.h"
#include "protocols/fs/client.hpp"

namespace {
struct Socket : File {
	Socket(helix::UniqueLane sock_lane)
		: File { StructName::get("socket") },
		_lane { std::move(sock_lane) } {
	}

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _lane;
	}

private:
	helix::UniqueLane _lane;
};
}

namespace extern_socket {
async::result<smarter::shared_ptr<File, FileHandle>> createSocket(helix::BorrowedLane lane, int type, int proto) {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;
	helix::PullDescriptor recv_lane;

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::CREATE_SOCKET);
	req.set_type(type);
	req.set_protocol(proto);
	auto req_data = req.SerializeAsString();
	char buffer[128];

	auto transmit = helix::submitAsync(lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, req_data.data(), req_data.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, sizeof(buffer), kHelItemChain),
			helix::action(&recv_lane));
	co_await transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_lane.error());

	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);

	co_return File::constructHandle(
		smarter::make_shared<Socket>(recv_lane.descriptor()));
}
}
