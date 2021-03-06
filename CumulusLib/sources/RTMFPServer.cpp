/* 
	Copyright 2010 OpenRTMFP
 
	This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License received along this program for more
	details (or else see http://www.gnu.org/licenses/).

	This file is a part of Cumulus.
*/

#include "RTMFPServer.h"
#include "RTMFP.h"
#include "Handshake.h"
#include "Middle.h"
#include "PacketWriter.h"
#include "Util.h"
#include "Logs.h"
#include "Poco/Format.h"
#include "string.h"


using namespace std;
using namespace Poco;
using namespace Poco::Net;


namespace Cumulus {

class RTMFPManager : private Task {
public:
	RTMFPManager(RTMFPServer& server):_server(server),Task(server,"RTMFPManager")  {
		setPriority(Thread::PRIO_LOW);
		start();
	}
	virtual ~RTMFPManager() {
		stop();
	}
	void run() {
		while(sleep(2000)!=STOP)
			waitHandle();
	}
private:
	void handle() {
		_server.manage();
	}
	RTMFPServer& _server;
};


RTMFPServer::RTMFPServer(UInt32 cores) : Startable("RTMFPServer"),_pRTMFPReceived(NULL),_pRTMFPReceiving(new RTMFPReceiving(*this)),_sendingEngine(cores),_receivingEngine(cores),_pCirrus(NULL),_handshake(_receivingEngine,_sendingEngine,*this,_edgesSocket,*this,*this),_sessions(*this) {
#ifndef POCO_OS_FAMILY_WINDOWS
//	static const char rnd_seed[] = "string to make the random number generator think it has entropy";
//	RAND_seed(rnd_seed, sizeof(rnd_seed));
#endif
	DEBUG("Id of this RTMFP server : %s",Util::FormatHex(id,ID_SIZE).c_str());
}

RTMFPServer::RTMFPServer(const string& name,UInt32 cores) : Startable(name),_pRTMFPReceived(NULL),_pRTMFPReceiving(new RTMFPReceiving(*this)),_sendingEngine(cores),_receivingEngine(cores),_pCirrus(NULL),_handshake(_receivingEngine,_sendingEngine,*this,_edgesSocket,*this,*this),_sessions(*this) {
#ifndef POCO_OS_FAMILY_WINDOWS
//	static const char rnd_seed[] = "string to make the random number generator think it has entropy";
//	RAND_seed(rnd_seed, sizeof(rnd_seed));
#endif
	DEBUG("Id of this RTMFP server : %s",Util::FormatHex(id,ID_SIZE).c_str());
}

RTMFPServer::~RTMFPServer() {
	stop();
}

void RTMFPServer::start() {
	RTMFPServerParams params;
	start(params);
}

void RTMFPServer::start(RTMFPServerParams& params) {
	if(running()) {
		ERROR("RTMFPServer server is yet running, call stop method before");
		return;
	}
	_port = params.port;
	if(_port==0) {
		ERROR("RTMFPServer port must have a positive value");
		return;
	}
	_edgesPort = params.edgesPort;
	if(_port==_edgesPort) {
		ERROR("RTMFPServer port must different than RTMFPServer edges.port");
		return;
	}
	if(params.pCirrus) {
		_pCirrus = new Target(*params.pCirrus);
		NOTE("RTMFPServer started in man-in-the-middle mode with server %s (unstable debug mode)",_pCirrus->address.toString().c_str());
	}
	_middle = params.middle;
	if(_middle)
		NOTE("RTMFPServer started in man-in-the-middle mode between peers (unstable debug mode)");

	(UInt32&)udpBufferSize = params.udpBufferSize==0 ? _socket.getReceiveBufferSize() : params.udpBufferSize;
	_socket.setReceiveBufferSize(udpBufferSize);_socket.setSendBufferSize(udpBufferSize);
	_edgesSocket.setReceiveBufferSize(udpBufferSize);_edgesSocket.setSendBufferSize(udpBufferSize);
	DEBUG("Socket buffer receving/sending size = %u/%u",udpBufferSize,udpBufferSize);

	(UInt32&)keepAliveServer = params.keepAliveServer<5 ? 5000 : params.keepAliveServer*1000;
	(UInt32&)keepAlivePeer = params.keepAlivePeer<5 ? 5000 : params.keepAlivePeer*1000;
	(UInt8&)edgesAttemptsBeforeFallback = params.edgesAttemptsBeforeFallback;
	
	setPriority(params.threadPriority);
	Startable::start();
}

void RTMFPServer::prerun() {
	NOTE("RTMFP server starts on %u port",_port);
	if(_edgesPort>0)
		NOTE("RTMFP edges server starts on %u port",_edgesPort);

	try {
		onStart();
		Startable::prerun();
	} catch(Exception& ex) {
		FATAL("RTMFPServer, %s",ex.displayText().c_str());
	} catch (exception& ex) {
		FATAL("RTMFPServer, %s",ex.what());
	} catch (...) {
		FATAL("RTMFPServer, unknown error");
	}
	onStop();
	
	NOTE("RTMFP server stops");
}

void RTMFPServer::run() {
	_socket.bind(SocketAddress("0.0.0.0",_port));
	_mainSockets.add(_socket,*this);

	if(_edgesPort>0) {
		_edgesSocket.bind(SocketAddress("0.0.0.0",_edgesPort));
		_mainSockets.add(_edgesSocket,*this);
	}

	{
		RTMFPManager manager(*this);
		bool terminate=false;
		while(!terminate)
			handle(terminate);
	}

	_handshake.clear();
	_sessions.clear();
	_sendingEngine.clear();
	_mainSockets.remove(_socket);
	_socket.close();
	_receivingEngine.clear();
	if(_edgesPort>0) {
		_mainSockets.remove(_edgesSocket);
		_edgesSocket.close();
	}

	if(_pCirrus) {
		delete _pCirrus;
		_pCirrus = NULL;
	}
}


void RTMFPServer::onError(const Poco::Net::Socket& socket,const std::string& error) {
	ERROR("RTMFPServer, %s",error.c_str());
}

void RTMFPServer::onReadable(Socket& socket) {
	PacketReader* pPacket = _pRTMFPReceiving->receive((DatagramSocket&)socket);
	if(!pPacket)
		return;

	_pRTMFPReceiving->id = RTMFP::Unpack(*pPacket);
	if(_pRTMFPReceiving->id==0) {
		_handshake.decode(_pRTMFPReceiving,socket==_edgesSocket ? AESEngine::EMPTY : AESEngine::DEFAULT);
		_pRTMFPReceiving = new RTMFPReceiving(*this);
		return;
	}
	ScopedLock<Mutex>  lock(_sessions.mutex);
	Session* pSession = _sessions.find(_pRTMFPReceiving->id);
	if(!pSession) {
		WARN("Unknown session %u",_pRTMFPReceiving->id);
		return;
	}
	pSession->decode(_pRTMFPReceiving);
	_pRTMFPReceiving = new RTMFPReceiving(*this);
}

void RTMFPServer::handle(bool& terminate){
	if(sleep()!=STOP) {
		// Process task
		giveHandle();

		ScopedLock<FastMutex> lock(_mutex);
		if(_pRTMFPReceived.isNull())
			return;
		// Process packet
		bool isEdges = _pRTMFPReceived->socket==_edgesSocket;
		if(isEdges) {
			Edge* pEdge = edges(_pRTMFPReceived->address);
			if(pEdge)
				pEdge->update();
		}

		if(_pRTMFPReceived->id==0) {
			DEBUG("Handshaking");
			_handshake.setEndPoint(_pRTMFPReceived->socket,_pRTMFPReceived->address);
			if(isEdges)
				_handshake.nextDumpAreMiddle=true;
			_handshake.receive(*_pRTMFPReceived->pPacket);
		} else {
			Session* pSession = _sessions.find(_pRTMFPReceived->id);
			if(pSession) {
				if(!pSession->checked)
					_handshake.commitCookie(*pSession);
				pSession->setEndPoint(_pRTMFPReceived->socket,_pRTMFPReceived->address);
				pSession->receive(*_pRTMFPReceived->pPacket);
			}
		}

		_pRTMFPReceived = NULL;

	} else
		terminate = true;
}

void RTMFPServer::receive(RTMFPReceiving& rtmfpReceiving) {
	ScopedLock<FastMutex> lock(_mutex);
	_pRTMFPReceived.assign(&rtmfpReceiving,true);
	wakeUp();
}

UInt8 RTMFPServer::p2pHandshake(const string& tag,PacketWriter& response,const SocketAddress& address,const UInt8* peerIdWanted) {

	// find the flash client equivalence
	Session* pSession = NULL;
	Sessions::Iterator it;
	for(it=_sessions.begin();it!=_sessions.end();++it) {
		pSession = it->second;
		if(Util::SameAddress(pSession->peer.address,address))
			break;
	}
	if(it==_sessions.end())
		pSession=NULL;

	ServerSession* pSessionWanted = (ServerSession*)_sessions.find(peerIdWanted);

	if(_pCirrus) {
		// Just to make working the man in the middle mode !

		if(!pSession) {
			ERROR("UDP Hole punching error : middle equivalence not found for session wanted");
			return 0;
		}

		PacketWriter& request = ((Middle*)pSession)->handshaker();
		request.write8(0x22);request.write8(0x21);
		request.write8(0x0F);
		request.writeRaw(pSessionWanted ? ((Middle*)pSessionWanted)->middlePeer().id : peerIdWanted,ID_SIZE);
		request.writeRaw(tag);

		((Middle*)pSession)->sendHandshakeToTarget(0x30);
		// no response here!
		return 0;
	}

	
	if(!pSessionWanted) {
		DEBUG("UDP Hole punching : session wanted not found, must be dead");
		return 0;
	} else if(pSessionWanted->failed()) {
		DEBUG("UDP Hole punching : session wanted is deleting");
		return 0;
	}

	UInt8 result = 0x00;
	if(_middle) {
		if(pSessionWanted->pTarget) {
			HelloAttempt& attempt = _handshake.helloAttempt<HelloAttempt>(tag);
			attempt.pTarget = pSessionWanted->pTarget;
			_handshake.createCookie(response,attempt,tag,"");
			response.writeRaw("\x81\x02\x1D\x02");
			response.writeRaw(pSessionWanted->pTarget->publicKey,KEY_SIZE);
			result = 0x70;
		} else
			ERROR("Peer/peer dumped exchange impossible : no corresponding 'Target' with the session wanted");
	}

	
	if(result==0x00) {
		/// Udp hole punching normal process
		pSessionWanted->p2pHandshake(address,tag,pSession);

		bool first=true;
		list<Address>::const_iterator it2;
		for(it2=pSessionWanted->peer.addresses.begin();it2!=pSessionWanted->peer.addresses.end();++it2) {
			const Address& addr = *it2;
			if(addr == address)
				WARN("A client tries to connect to himself (same %s address)",address.toString().c_str());
			response.writeAddress(addr,first);
			DEBUG("P2P address initiator exchange, %s:%u",Util::FormatHex(&addr.host[0],addr.host.size()).c_str(),addr.port);
			first=false;
		}

		result = 0x71;
	}

	return result;

}

Session& RTMFPServer::createSession(UInt32 farId,const Peer& peer,const UInt8* decryptKey,const UInt8* encryptKey,Cookie& cookie) {

	Target* pTarget=_pCirrus;

	if(_middle) {
		if(!cookie.pTarget) {
			cookie.pTarget = new Target(peer.address,&cookie);
			memcpy((UInt8*)cookie.pTarget->peerId,peer.id,ID_SIZE);
			memcpy((UInt8*)peer.id,cookie.pTarget->id,ID_SIZE);
			NOTE("Mode 'man in the middle' : to connect to peer '%s' use the id :\n%s",Util::FormatHex(cookie.pTarget->peerId,ID_SIZE).c_str(),Util::FormatHex(cookie.pTarget->id,ID_SIZE).c_str());
		} else
			pTarget = cookie.pTarget;
	}

	ServerSession* pSession;
	if(pTarget) {
		pSession = new Middle(_receivingEngine,_sendingEngine,_sessions.nextId(),farId,peer,decryptKey,encryptKey,*this,_sessions,*pTarget);
		if(_pCirrus==pTarget)
			pSession->pTarget = cookie.pTarget;
		DEBUG("Wait cirrus handshaking");
		pSession->manage(); // to wait the cirrus handshake
	} else {
		pSession = new ServerSession(_receivingEngine,_sendingEngine,_sessions.nextId(),farId,peer,decryptKey,encryptKey,*this);
		pSession->pTarget = cookie.pTarget;
	}

	_sessions.add(pSession);

	return *pSession;
}

void RTMFPServer::destroySession(Session& session) {
	if(session.flags&SESSION_BY_EDGE) {
		Edge* pEdge = edges(session.peer.address);
		if(pEdge)
			pEdge->removeSession(session);
	}
}

void RTMFPServer::manage() {
	_handshake.manage();
	_sessions.manage();
}



} // namespace Cumulus
