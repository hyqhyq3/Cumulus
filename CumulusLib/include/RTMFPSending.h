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

#pragma once

#include "Cumulus.h"
#include "AESEngine.h"
#include "PacketWriter.h"
#include "Poco/RefCountedObject.h"
#include "Poco/Net/DatagramSocket.h"

namespace Cumulus {

class RTMFPSending : public Poco::RefCountedObject {
public:
	RTMFPSending();
	~RTMFPSending();

	Poco::UInt32				id;
	Poco::UInt32				farId;
	Poco::Net::DatagramSocket	socket;
	AESEngine					encoder;
	Poco::Net::SocketAddress	address;
	PacketWriter				packet;

	void						run();
private:
	Poco::UInt8					_buffer[PACKETSEND_SIZE];
};

} // namespace Cumulus
