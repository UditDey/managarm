
#include "kernel.hpp"

namespace thor {

// --------------------------------------------------------
// Channel
// --------------------------------------------------------

Channel::Channel()
: _readEndpointClosed(false), _writeEndpointClosed(false) { }

Channel::~Channel() {
	assert(_readEndpointClosed);
	assert(_writeEndpointClosed);
}

Error Channel::sendString(Guard &guard, frigg::SharedPtr<AsyncSendString> send) {
	assert(guard.protects(&lock));

	assert(!_writeEndpointClosed);
	if(_readEndpointClosed)
		return kErrClosedRemotely;

	bool queue_message = true;
	for(auto it = _recvStringQueue.frontIter(); it; ++it) {
		if(!matchStringRequest(send, *it))
			continue;
		
		if(processStringRequest(send, (*it).toShared())) {
			_recvStringQueue.remove(it);
			// don't queue the message if a request succeeds
			queue_message = false;
			break;
		}
	}

	if(queue_message)
		_sendStringQueue.addBack(frigg::move(send));
	return kErrSuccess;
}

Error Channel::sendDescriptor(Guard &guard, frigg::SharedPtr<AsyncSendDescriptor> send) {
	assert(guard.protects(&lock));

	assert(!_writeEndpointClosed);
	if(_readEndpointClosed)
		return kErrClosedRemotely;

	for(auto it = _recvDescriptorQueue.frontIter(); it; ++it) {
		if(!matchDescriptorRequest(send, *it))
			continue;
		
		processDescriptorRequest(send, (*it).toShared());
		_recvDescriptorQueue.remove(it);
		return kErrSuccess;
	}

	_sendDescriptorQueue.addBack(frigg::move(send));
	return kErrSuccess;
}

Error Channel::submitRecvString(Guard &guard, frigg::SharedPtr<AsyncRecvString> recv) {
	assert(guard.protects(&lock));

	assert(!_readEndpointClosed);
	if(_writeEndpointClosed)
		return kErrClosedRemotely;

	bool queue_request = true;
	for(auto it = _sendStringQueue.frontIter(); it; ++it) {
		if(!matchStringRequest(*it, recv))
			continue;
		
		if(processStringRequest((*it).toShared(), recv))
			_sendStringQueue.remove(it);
		// NOTE: we never queue failed requests
		queue_request = false;
		break;
	}
	
	if(queue_request)
		_recvStringQueue.addBack(frigg::move(recv));
	return kErrSuccess;
}

Error Channel::submitRecvDescriptor(Guard &guard, frigg::SharedPtr<AsyncRecvDescriptor> recv) {
	assert(guard.protects(&lock));

	assert(!_readEndpointClosed);
	if(_writeEndpointClosed)
		return kErrClosedRemotely;

	for(auto it = _sendDescriptorQueue.frontIter(); it; ++it) {
		if(!matchDescriptorRequest(*it, recv))
			continue;
		
		processDescriptorRequest((*it).toShared(), recv);
		_sendDescriptorQueue.remove(it);
		return kErrSuccess;
	}
	
	_recvDescriptorQueue.addBack(frigg::move(recv));
	return kErrSuccess;
}

void Channel::closeReadEndpoint(Guard &guard) {
	assert(guard.protects(&lock));
	assert(!_readEndpointClosed);
	_readEndpointClosed = true;
	
	// drain the send queues
	while(!_sendStringQueue.empty()) {
		frigg::SharedPtr<AsyncSendString> send = _sendStringQueue.removeFront();
		send->error = kErrClosedRemotely;
		AsyncOperation::complete(frigg::move(send));
	}
	
	while(!_sendDescriptorQueue.empty()) {
		frigg::SharedPtr<AsyncSendDescriptor> send = _sendDescriptorQueue.removeFront();
		send->error = kErrClosedRemotely;
		AsyncOperation::complete(frigg::move(send));
	}

	// drain the receive queues
	while(!_recvStringQueue.empty()) {
		frigg::SharedPtr<AsyncRecvString> send = _recvStringQueue.removeFront();
		send->error = kErrClosedLocally;
		AsyncOperation::complete(frigg::move(send));
	}
	
	while(!_recvDescriptorQueue.empty()) {
		frigg::SharedPtr<AsyncRecvDescriptor> send = _recvDescriptorQueue.removeFront();
		send->error = kErrClosedLocally;
		AsyncOperation::complete(frigg::move(send));
	}
}

void Channel::closeWriteEndpoint(Guard &guard) {
	assert(guard.protects(&lock));
	assert(!_writeEndpointClosed);
	_writeEndpointClosed = true;
	
	// drain the send queues
	while(!_sendStringQueue.empty()) {
		frigg::SharedPtr<AsyncSendString> send = _sendStringQueue.removeFront();
		send->error = kErrClosedLocally;
		AsyncOperation::complete(frigg::move(send));
	}
	
	while(!_sendDescriptorQueue.empty()) {
		frigg::SharedPtr<AsyncSendDescriptor> send = _sendDescriptorQueue.removeFront();
		send->error = kErrClosedLocally;
		AsyncOperation::complete(frigg::move(send));
	}
	
	// drain the receive queues
	while(!_recvStringQueue.empty()) {
		frigg::SharedPtr<AsyncRecvString> send = _recvStringQueue.removeFront();
		send->error = kErrClosedRemotely;
		AsyncOperation::complete(frigg::move(send));
	}
	
	while(!_recvDescriptorQueue.empty()) {
		frigg::SharedPtr<AsyncRecvDescriptor> send = _recvDescriptorQueue.removeFront();
		send->error = kErrClosedRemotely;
		AsyncOperation::complete(frigg::move(send));
	}
}

bool Channel::matchStringRequest(frigg::UnsafePtr<AsyncSendString> send,
		frigg::UnsafePtr<AsyncRecvString> recv) {
	if((bool)(recv->flags & kFlagRequest) != (bool)(send->flags & kFlagRequest))
		return false;
	if((bool)(recv->flags & kFlagResponse) != (bool)(send->flags & kFlagResponse))
		return false;
	
	if(recv->filterRequest != -1)
		if(recv->filterRequest != send->msgRequest)
			return false;
	
	if(recv->filterSequence != -1)
		if(recv->filterSequence != send->msgSequence)
			return false;
	
	return true;
}

bool Channel::matchDescriptorRequest(frigg::UnsafePtr<AsyncSendDescriptor> send,
		frigg::UnsafePtr<AsyncRecvDescriptor> recv) {
	if((bool)(recv->flags & kFlagRequest) != (bool)(send->flags & kFlagRequest))
		return false;
	if((bool)(recv->flags & kFlagResponse) != (bool)(send->flags & kFlagResponse))
		return false;
	
	if(recv->filterRequest != -1)
		if(recv->filterRequest != send->msgRequest)
			return false;
	
	if(recv->filterSequence != -1)
		if(recv->filterSequence != send->msgSequence)
			return false;
	
	return true;
}

bool Channel::processStringRequest(frigg::SharedPtr<AsyncSendString> send,
		frigg::SharedPtr<AsyncRecvString> recv) {
	if(recv->type == AsyncRecvString::kTypeNormal) {
		if(send->kernelBuffer.size() <= recv->spaceLock.length()) {
			// perform the actual data transfer
			recv->spaceLock.copyTo(send->kernelBuffer.data(), send->kernelBuffer.size());
			
			send->error = kErrSuccess;

			recv->error = kErrSuccess;
			recv->msgRequest = send->msgRequest;
			recv->msgSequence = send->msgSequence;
			recv->length = send->kernelBuffer.size();

			AsyncOperation::complete(frigg::move(send));
			AsyncOperation::complete(frigg::move(recv));
			return true;
		}else{
			// post the error event
			assert(!"Fix recv error");
			{
/*				UserEvent event(UserEvent::kTypeError, recv->submitInfo);
				event.error = kErrBufferTooSmall;

				frigg::SharedPtr<EventHub> event_hub = recv->eventHub.grab();
				assert(event_hub);
				EventHub::Guard hub_guard(&event_hub->lock);
				event_hub->raiseEvent(hub_guard, frigg::move(event));*/
			}
			return false;
		}
	}else if(recv->type == AsyncRecvString::kTypeToRing) {
		// transfer the request to the ring buffer
		frigg::SharedPtr<RingBuffer> ring_buffer(recv->ringBuffer);
		ring_buffer->doTransfer(frigg::move(send), frigg::move(recv));
		return true;
	}else{
		frigg::panicLogger() << "Illegal request type" << frigg::endLog;
		__builtin_unreachable();
	}
}

void Channel::processDescriptorRequest(frigg::SharedPtr<AsyncSendDescriptor> send,
		frigg::SharedPtr<AsyncRecvDescriptor> recv) {
	frigg::SharedPtr<Universe> universe = recv->universe.grab();
	assert(universe);

	Handle handle;
	{
		Universe::Guard universe_guard(&universe->lock);
		handle = universe->attachDescriptor(universe_guard,
				frigg::move(send->descriptor));
	}

	send->error = kErrSuccess;

	recv->error = kErrSuccess;
	recv->msgRequest = send->msgRequest;
	recv->msgSequence = send->msgSequence;
	recv->handle = handle;

	AsyncOperation::complete(frigg::move(send));
	AsyncOperation::complete(frigg::move(recv));
}

} // namespace thor
