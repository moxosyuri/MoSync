/* Copyright 2013 David Axmark

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

	http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#ifndef NETWORKING_H
#error Bad inclusion!
#endif	//NETWORKING_H

#include <string>

#include <helpers/hash_map.h>

#include <helpers/cpp_defs.h>
#include <helpers/smartie.h>

#include <bluetooth/connection.h>
#include <bluetooth/server.h>

#include "MemStream.h"

#include "TcpConnection.h"
#include "ThreadPool.h"
#include "netImpl.h"

using namespace Base;
using namespace MoSyncError;

//***************************************************************************
//Implementation declarations
//***************************************************************************

void ConnWaitEvent();
void ConnPushEvent(MAEvent* ep);
void DefluxBinPushEvent(MAHandle handle, Stream& s);
namespace Base {
	bool MAProcessEvents();
}
void MANetworkSslInit();
void MANetworkSslClose();

//***************************************************************************
//Variables
//***************************************************************************

struct MAConn;
typedef std::pair<MAHandle, MAConn*> ConnPair;
typedef hash_map<MAHandle, MAConn*> ConnMap;
typedef ConnMap::iterator ConnItr;

static const char rtsp_string[] = "rtsp://";

extern MoSyncMutex* gpConnMutex;
#define gConnMutex (*gpConnMutex)

extern ConnMap* gpConnections;
#define gConnections (*gpConnections)

extern int gConnNextHandle;

//***************************************************************************
//Glue classes, MAConn
//***************************************************************************

enum MACType {
	eStreamConn, eServerConn
};

struct MAConn {
	MAConn(MAHandle h, MACType t, Closable* c) : handle(h), type(t), clo(c),
		state(0), cancel(false) {}

	void close() {
		cancel = true;
		clo->close();	//should disrupt any ongoing ops
		clo = NULL;

		//wait on ops
		while(true) {
			gConnMutex.lock();
			{
				MAProcessEvents();
				if(state == 0) {
					gConnMutex.unlock();
					break;
				}
			}
			gConnMutex.unlock();

			ConnWaitEvent();	//wait until there are events to be processed
		}
	}

	const MAHandle handle;
	const MACType type;
	Closable* clo;
	int state;
	bool cancel;
};

struct MAStreamConn : public MAConn {
	MAStreamConn(MAHandle h, Connection* c) : MAConn(h, eStreamConn, c), conn(c) {}
	Connection* conn;
};

struct MAServerConn : public MAConn {
	MAServerConn(MAHandle h, BtSppServer* s) : MAConn(h, eServerConn, s), serv(s) {}
	BtSppServer* serv;
};

//***************************************************************************
//Glue classes, ConnOp
//***************************************************************************

class ConnOp : public Runnable {
protected:
	ConnOp(MAConn& m) : mac(m) {}
	MAConn& mac;

	void handleResult(int opcode, int result, bool lock = true) {
		LOGST("ConnOp::handleResult %i %i %i", mac.handle, opcode, result);
		if(lock)
		{
			gConnMutex.lock();
		}
        if(result < 0 && mac.cancel) {
			result = CONNERR_CANCELED;
		}
		DEBUG_ASSERT(mac.state & opcode);

		MAEvent* ep = new MAEvent;
		ep->type = EVENT_TYPE_CONN;
		ep->conn.handle = mac.handle;
		ep->conn.opType = opcode;
		ep->conn.result = result;

		mac.state &= ~opcode;

		ConnPushEvent(ep);	//send event to be processed
		if(lock)
		{
			gConnMutex.unlock();
		}
	}
};

class ConnStreamOp : public ConnOp {
public:
	ConnStreamOp(MAStreamConn& m) : ConnOp(m), masc(m) {}
protected:
	MAStreamConn& masc;
};

class Connect : public ConnStreamOp {
public:
	Connect(MAStreamConn& m) : ConnStreamOp(m) {}
	void run() {
		LOGST("Connect %i", mac.handle);
        handleResult(CONNOP_CONNECT, masc.conn->connect());
	}
};

class ConnRead : public ConnStreamOp {
public:
	ConnRead(MAStreamConn& m, void* d, int s) : ConnStreamOp(m), dst(d), size(s) {}
	void run() {
		LOGST("ConnRead %i", mac.handle);
        handleResult(CONNOP_READ, masc.conn->read(dst, size));
	}
private:
	void* dst;
	const int size;
};

class ConnReadFrom : public ConnStreamOp {
public:
	ConnReadFrom(MAStreamConn& m, void* d, int s, MAConnAddr* a) : ConnStreamOp(m), dst(d), size(s), src(a) {}
	void run() {
		LOGST("ConnReadFrom %i", mac.handle);
		handleResult(CONNOP_READ, masc.conn->readFrom(dst, size, *src));
	}
private:
	void* dst;
	const int size;
	MAConnAddr* src;
};

class ConnWrite : public ConnStreamOp {
public:
	ConnWrite(MAStreamConn& m, const void* sr, int si) : ConnStreamOp(m), src(sr), size(si) {}
	void run() {
		LOGST("ConnWrite %i", mac.handle);
        handleResult(CONNOP_WRITE, masc.conn->write(src, size));
	}
private:
	const void* src;
	const int size;
};

class ConnWriteTo : public ConnStreamOp {
public:
	ConnWriteTo(MAStreamConn& m, const void* sr, int si, const MAConnAddr& d) : ConnStreamOp(m), src(sr), size(si), dst(d) {}
	void run() {
		LOGST("ConnWriteTo %i", mac.handle);
		handleResult(CONNOP_WRITE, masc.conn->writeTo(src, size, dst));
	}
private:
	const void* src;
	const int size;
	const MAConnAddr& dst;
};

class ConnReadToData : public ConnStreamOp {
public:
	ConnReadToData(MAStreamConn& m, MemStream& d, MAHandle h, int o, int s)
		: ConnStreamOp(m), dst(d), handle(h), offset(o), size(s) {}
	void run() {
		LOGST("ConnReadToData %i", mac.handle);
		int result = masc.conn->read((byte*)dst.ptr() + offset, size);
        gConnMutex.lock();
        {
            DefluxBinPushEvent(handle, dst);

            handleResult(CONNOP_READ, result, false);
        }
        gConnMutex.unlock();
	}
private:
	MemStream& dst;
	const MAHandle handle;
	const int offset;
	const int size;
};

class ConnWriteFromData : public ConnStreamOp {
public:
	ConnWriteFromData(MAStreamConn& m, Stream& sr, MAHandle h, int o, int si)
		: ConnStreamOp(m), src(sr), handle(h), offset(o), size(si) {}
	void run() {
		LOGST("ConnWriteFromData %i", mac.handle);

        int result;
        if(src.ptrc() != NULL) {
            result = masc.conn->write((byte*)src.ptrc() + offset, size);
        } else {
            Smartie<byte> temp(new byte[size]);
            if(!src.read(temp(), size)) {
                LOG("Stream error in ConnWriteFromData!\n");
                result = CONNERR_GENERIC;
            } else {
                result = masc.conn->write(temp(), size);
            }
        }
		gConnMutex.lock();
        {
            DefluxBinPushEvent(handle, src);

            handleResult(CONNOP_WRITE, result, false);
        }
        gConnMutex.unlock();

	}
private:
	Stream& src;
	const MAHandle handle;
	const int offset;
	const int size;
};

class HttpFinish : public ConnOp {
public:
	HttpFinish(MAConn& m, HttpConnection& h) : ConnOp(m), http(h) {}
	void run() {
		LOGST("HttpFinish %i", mac.handle);
        handleResult(CONNOP_FINISH, http.finish());
	}
private:
	HttpConnection& http;
};

class Accept : public ConnOp {
public:
	Accept(MAServerConn& m) : ConnOp(m), masc(m) {}
	void run() {
		LOGST("Accept %i\n", mac.handle);
        BtSppConnection* conn;

		int res = masc.serv->accept(conn);
		if(res < 0) {
			handleResult(CONNOP_ACCEPT, res);
			return;
        }
            //success. let's store our new connection.
		MAConn* newMac = new MAStreamConn(gConnNextHandle, conn);
		gConnections.insert(ConnPair(gConnNextHandle, newMac));
		handleResult(CONNOP_ACCEPT, gConnNextHandle++);
	}
private:
	MAServerConn& masc;
};

//***************************************************************************
//Functions
//***************************************************************************

void MANetworkInit();
void MANetworkReset();
void MANetworkClose();
