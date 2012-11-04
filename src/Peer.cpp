#include <iostream>

#include <boost/bind.hpp>
#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <boost/ref.hpp>

#include "../json/writer.h"

#include "Version.h"
#include "Peer.h"
#include "Config.h"
#include "Application.h"
#include "SerializedTransaction.h"
#include "utils.h"
#include "Log.h"

SETUP_LOG();
DECLARE_INSTANCE(Peer);

// Don't try to run past receiving nonsense from a peer
#define TRUST_NETWORK

// Node has this long to verify its identity from connection accepted or connection attempt.
#define NODE_VERIFY_SECONDS		15

Peer::Peer(boost::asio::io_service& io_service, boost::asio::ssl::context& ctx, uint64 peerID) :
	mHelloed(false),
	mDetaching(false),
	mPeerId(peerID),
	mSocketSsl(io_service, ctx),
	mVerifyTimer(io_service)
{
	cLog(lsDEBUG) << "CREATING PEER: " << ADDRESS(this);
}

void Peer::handle_write(const boost::system::error_code& error, size_t bytes_transferred)
{
#ifdef DEBUG
//	if (!error)
//		std::cerr << "Peer::handle_write bytes: "<< bytes_transferred << std::endl;
#endif

	mSendingPacket.reset();

	if (mDetaching)
	{
		// Ignore write requests when detatching.
		nothing();
	}
	else if (error)
	{
		cLog(lsINFO) << "Peer: Write: Error: " << ADDRESS(this) << ": bytes=" << bytes_transferred << ": " << error.category().name() << ": " << error.message() << ": " << error;

		detach("hw");
	}
	else if (!mSendQ.empty())
	{
		PackedMessage::pointer packet = mSendQ.front();

		if (packet)
		{
			sendPacketForce(packet);
			mSendQ.pop_front();
		}
	}
}

void Peer::setIpPort(const std::string& strIP, int iPort)
{
	mIpPort = make_pair(strIP, iPort);

	cLog(lsDEBUG) << "Peer: Set: "
		<< ADDRESS(this) << "> "
		<< (mNodePublic.isValid() ? mNodePublic.humanNodePublic() : "-") << " " << getIP() << " " << getPort();
}

void Peer::detach(const char *rsn)
{
	if (!mDetaching)
	{
		mDetaching	= true;			// Race is ok.
		/*
		cLog(lsDEBUG) << "Peer: Detach: "
			<< ADDRESS(this) << "> "
			<< rsn << ": "
			<< (mNodePublic.isValid() ? mNodePublic.humanNodePublic() : "-") << " " << getIP() << " " << getPort();
			*/

		mSendQ.clear();

		(void) mVerifyTimer.cancel();
		mSocketSsl.async_shutdown(boost::bind(&Peer::sHandleShutdown, shared_from_this(), boost::asio::placeholders::error));

		if (mNodePublic.isValid())
		{
			theApp->getConnectionPool().peerDisconnected(shared_from_this(), mNodePublic);

			mNodePublic.clear();		// Be idempotent.
		}

		if (!mIpPort.first.empty())
		{
			// Connection might be part of scanning.  Inform connect failed.
			// Might need to scan. Inform connection closed.
			theApp->getConnectionPool().peerClosed(shared_from_this(), mIpPort.first, mIpPort.second);

			mIpPort.first.clear();		// Be idempotent.
		}
		/*
		cLog(lsDEBUG) << "Peer: Detach: "
			<< ADDRESS(this) << "< "
			<< rsn << ": "
			<< (mNodePublic.isValid() ? mNodePublic.humanNodePublic() : "-") << " " << getIP() << " " << getPort();
			*/
	}
}

void Peer::handleVerifyTimer(const boost::system::error_code& ecResult)
{
	if (ecResult == boost::asio::error::operation_aborted)
	{
		// Timer canceled because deadline no longer needed.
		// std::cerr << "Deadline cancelled." << std::endl;

		nothing(); // Aborter is done.
	}
	else if (ecResult)
	{
		cLog(lsINFO) << "Peer verify timer error";

		// Can't do anything sound.
		abort();
	}
	else
	{
		//cLog(lsINFO) << "Peer: Verify: Peer failed to verify in time.";

		detach("hvt");
	}
}

// Begin trying to connect. We are not connected till we know and accept peer's public key.
// Only takes IP addresses (not domains).
void Peer::connect(const std::string& strIp, int iPort)
{
	int	iPortAct	= (iPort <= 0) ? SYSTEM_PEER_PORT : iPort;

	mClientConnect	= true;

	mIpPort			= make_pair(strIp, iPort);
	mIpPortConnect	= mIpPort;
	assert(!mIpPort.first.empty());

	boost::asio::ip::tcp::resolver::query	query(strIp, boost::lexical_cast<std::string>(iPortAct),
			boost::asio::ip::resolver_query_base::numeric_host|boost::asio::ip::resolver_query_base::numeric_service);
	boost::asio::ip::tcp::resolver				resolver(theApp->getIOService());
	boost::system::error_code					err;
	boost::asio::ip::tcp::resolver::iterator	itrEndpoint	= resolver.resolve(query, err);

	if (err || itrEndpoint == boost::asio::ip::tcp::resolver::iterator())
	{
		cLog(lsWARNING) << "Peer: Connect: Bad IP: " << strIp;
		detach("c");
		return;
	}
	else
	{
		mVerifyTimer.expires_from_now(boost::posix_time::seconds(NODE_VERIFY_SECONDS), err);
		mVerifyTimer.async_wait(boost::bind(&Peer::sHandleVerifyTimer, shared_from_this(), boost::asio::placeholders::error));

		if (err)
		{
			cLog(lsWARNING) << "Peer: Connect: Failed to set timer.";
			detach("c2");
			return;
		}
	}

	if (!err)
	{
		cLog(lsINFO) << "Peer: Connect: Outbound: " << ADDRESS(this) << ": " << mIpPort.first << " " << mIpPort.second;

		boost::asio::async_connect(
			getSocket(),
			itrEndpoint,
			boost::bind(
				&Peer::sHandleConnect,
				shared_from_this(),
				boost::asio::placeholders::error,
				boost::asio::placeholders::iterator));
	}
}

// We have an encrypted connection to the peer.
// Have it say who it is so we know to avoid redundant connections.
// Establish that it really who we are talking to by having it sign a connection detail.
// Also need to establish no man in the middle attack is in progress.
void Peer::handleStart(const boost::system::error_code& error)
{
	if (error)
	{
		cLog(lsINFO) << "Peer: Handshake: Error: " << error.category().name() << ": " << error.message() << ": " << error;
		detach("hs");
	}
	else
	{
		sendHello();			// Must compute mCookieHash before receiving a hello.
		start_read_header();
	}
}

// Connect ssl as client.
void Peer::handleConnect(const boost::system::error_code& error, boost::asio::ip::tcp::resolver::iterator it)
{
	if (error)
	{
		cLog(lsINFO) << "Peer: Connect: Error: " << error.category().name() << ": " << error.message() << ": " << error;
		detach("hc");
	}
	else
	{
		cLog(lsINFO) << "Connect peer: success.";

		mSocketSsl.set_verify_mode(boost::asio::ssl::verify_none);

		mSocketSsl.async_handshake(boost::asio::ssl::stream<boost::asio::ip::tcp::socket>::client,
			boost::bind(&Peer::sHandleStart, shared_from_this(), boost::asio::placeholders::error));
	}
}

// Connect ssl as server to an inbound connection.
// - We don't bother remembering the inbound IP or port.  Only useful for debugging.
void Peer::connected(const boost::system::error_code& error)
{
	boost::asio::ip::tcp::endpoint	ep		= getSocket().remote_endpoint();
	int								iPort	= ep.port();
	std::string						strIp	= ep.address().to_string();

	mClientConnect	= false;
	mIpPortConnect	= make_pair(strIp, iPort);

	if (iPort == SYSTEM_PEER_PORT)		//TODO: Why are you doing this?
		iPort	= -1;

	if (!error)
	{
		// Not redundant ip and port, handshake, and start.

		cLog(lsINFO) << "Peer: Inbound: Accepted: " << ADDRESS(this) << ": " << strIp << " " << iPort;

		mSocketSsl.set_verify_mode(boost::asio::ssl::verify_none);

		mSocketSsl.async_handshake(boost::asio::ssl::stream<boost::asio::ip::tcp::socket>::server,
			boost::bind(&Peer::sHandleStart, shared_from_this(), boost::asio::placeholders::error));
	}
	else if (!mDetaching)
	{
		cLog(lsINFO) << "Peer: Inbound: Error: " << ADDRESS(this) << ": " << strIp << " " << iPort << " : " << error.category().name() << ": " << error.message() << ": " << error;

		detach("ctd");
	}
}

void Peer::sendPacketForce(const PackedMessage::pointer& packet)
{
	if (!mDetaching)
	{
		mSendingPacket = packet;

		boost::asio::async_write(mSocketSsl, boost::asio::buffer(packet->getBuffer()),
			boost::bind(&Peer::sHandle_write, shared_from_this(),
			boost::asio::placeholders::error,
			boost::asio::placeholders::bytes_transferred));
	}
}

void Peer::sendPacket(const PackedMessage::pointer& packet)
{
	if (packet)
	{
		if (mSendingPacket)
		{
			mSendQ.push_back(packet);
		}
		else
		{
			sendPacketForce(packet);
		}
	}
}

void Peer::start_read_header()
{
	if (!mDetaching)
	{
		mReadbuf.clear();
		mReadbuf.resize(HEADER_SIZE);

		boost::asio::async_read(mSocketSsl, boost::asio::buffer(mReadbuf),
			boost::bind(&Peer::sHandle_read_header, shared_from_this(), boost::asio::placeholders::error));
	}
}

void Peer::start_read_body(unsigned msg_len)
{
	// m_readbuf already contains the header in its first HEADER_SIZE
	// bytes. Expand it to fit in the body as well, and start async
	// read into the body.

	if (!mDetaching)
	{
		mReadbuf.resize(HEADER_SIZE + msg_len);

		boost::asio::async_read(mSocketSsl, boost::asio::buffer(&mReadbuf[HEADER_SIZE], msg_len),
			boost::bind(&Peer::sHandle_read_body, shared_from_this(), boost::asio::placeholders::error));
	}
}

void Peer::handle_read_header(const boost::system::error_code& error)
{
	if (mDetaching)
	{
		// Drop data or error if detaching.
		nothing();
	}
	else if (!error)
	{
		unsigned msg_len = PackedMessage::getLength(mReadbuf);
		// WRITEME: Compare to maximum message length, abort if too large
		if ((msg_len > (32 * 1024 * 1024)) || (msg_len == 0))
		{
			detach("hrh");
			return;
		}
		start_read_body(msg_len);
	}
	else
	{
		cLog(lsINFO) << "Peer: Header: Error: " << ADDRESS(this) << ": " << error.category().name() << ": " << error.message() << ": " << error;
		detach("hrh2");
	}
}

void Peer::handle_read_body(const boost::system::error_code& error)
{
	if (mDetaching)
	{
		// Drop data or error if detaching.
		nothing();
	}
	else if (!error)
	{
		processReadBuffer();
		start_read_header();
	}
	else
	{
		cLog(lsINFO) << "Peer: Body: Error: " << ADDRESS(this) << ": " << error.category().name() << ": " << error.message() << ": " << error;
		detach("hrb");
	}
}

void Peer::processReadBuffer()
{
	int type = PackedMessage::getType(mReadbuf);
#ifdef DEBUG
//	std::cerr << "PRB(" << type << "), len=" << (mReadbuf.size()-HEADER_SIZE) << std::endl;
#endif

//	std::cerr << "Peer::processReadBuffer: " << mIpPort.first << " " << mIpPort.second << std::endl;

	// If connected and get a mtHELLO or if not connected and get a non-mtHELLO, wrong message was sent.
	if (mHelloed == (type == ripple::mtHELLO))
	{
		cLog(lsWARNING) << "Wrong message type: " << type;
		detach("prb1");
	}
	else
	{
		switch(type)
		{
		case ripple::mtHELLO:
			{
				ripple::TMHello msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvHello(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtERROR_MSG:
			{
				ripple::TMErrorMsg msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvErrorMessage(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtPING:
			{
				ripple::TMPing msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvPing(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtGET_CONTACTS:
			{
				ripple::TMGetContacts msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvGetContacts(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtCONTACT:
			{
				ripple::TMContact msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvContact(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;
		case ripple::mtGET_PEERS:
			{
				ripple::TMGetPeers msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvGetPeers(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;
		case ripple::mtPEERS:
			{
				ripple::TMPeers msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvPeers(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtSEARCH_TRANSACTION:
			{
				ripple::TMSearchTransaction msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvSearchTransaction(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtGET_ACCOUNT:
			{
				ripple::TMGetAccount msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvGetAccount(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtACCOUNT:
			{
				ripple::TMAccount msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvAccount(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtTRANSACTION:
			{
				ripple::TMTransaction msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvTransaction(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtSTATUS_CHANGE:
			{
				ripple::TMStatusChange msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvStatus(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtPROPOSE_LEDGER:
			{
				ripple::TMProposeSet msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvPropose(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtGET_LEDGER:
			{
				ripple::TMGetLedger msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvGetLedger(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtLEDGER_DATA:
			{
				ripple::TMLedgerData msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvLedger(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtHAVE_SET:
			{
				ripple::TMHaveTransactionSet msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvHaveTxSet(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		case ripple::mtVALIDATION:
			{
				boost::shared_ptr<ripple::TMValidation> msg = boost::make_shared<ripple::TMValidation>();
				if (msg->ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvValidation(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;
#if 0
		case ripple::mtGET_VALIDATION:
			{
				ripple::TM msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recv(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

#endif
		case ripple::mtGET_OBJECTS:
			{
				ripple::TMGetObjectByHash msg;
				if (msg.ParseFromArray(&mReadbuf[HEADER_SIZE], mReadbuf.size() - HEADER_SIZE))
					recvGetObjectByHash(msg);
				else
					cLog(lsWARNING) << "parse error: " << type;
			}
			break;

		default:
			cLog(lsWARNING) << "Unknown Msg: " << type;
			cLog(lsWARNING) << strHex(&mReadbuf[0], mReadbuf.size());
		}
	}
}

void Peer::punishPeer(const boost::weak_ptr<Peer>& wp, PeerPunish pp)
{
	Peer::pointer p = wp.lock();
	if (p)
		p->punishPeer(pp);
}

void Peer::recvHello(ripple::TMHello& packet)
{
	bool	bDetach	= true;

	// Cancel verification timeout.
	(void) mVerifyTimer.cancel();

	uint32 ourTime = theApp->getOPs().getNetworkTimeNC();
	uint32 minTime = ourTime - 20;
	uint32 maxTime = ourTime + 20;

#ifdef DEBUG
	if (packet.has_nettime())
	{
		int64 to = ourTime;
		to -= packet.nettime();
		cLog(lsDEBUG) << "Connect: time offset " << to;
	}
#endif

	if (packet.has_nettime() && ((packet.nettime() < minTime) || (packet.nettime() > maxTime)))
	{
		if (packet.nettime() > maxTime)
		{
			cLog(lsINFO) << "Recv(Hello): " << getIP() << " :Clock far off +" << packet.nettime() - ourTime;
		}
		else if(packet.nettime() < minTime)
		{
			cLog(lsINFO) << "Recv(Hello): " << getIP() << " :Clock far off -" << ourTime - packet.nettime();
		}
	}
	else if (packet.protoversionmin() < MAKE_VERSION_INT(MIN_PROTO_MAJOR, MIN_PROTO_MINOR))
	{
		cLog(lsINFO) << "Recv(Hello): Server requires protocol version " <<
			GET_VERSION_MAJOR(packet.protoversion()) << "." << GET_VERSION_MINOR(packet.protoversion())
				<< " we run " << PROTO_VERSION_MAJOR << "." << PROTO_VERSION_MINOR;
	}
	else if (!mNodePublic.setNodePublic(packet.nodepublic()))
	{
		cLog(lsINFO) << "Recv(Hello): Disconnect: Bad node public key.";
	}
	else if (!mNodePublic.verifyNodePublic(mCookieHash, packet.nodeproof()))
	{ // Unable to verify they have private key for claimed public key.
		cLog(lsINFO) << "Recv(Hello): Disconnect: Failed to verify session.";
	}
	else
	{ // Successful connection.
		cLog(lsINFO) << "Recv(Hello): Connect: " << mNodePublic.humanNodePublic();
		tLog(packet.protoversion() != MAKE_VERSION_INT(PROTO_VERSION_MAJOR, PROTO_VERSION_MINOR), lsINFO)
			<< "Peer speaks version " <<
				(packet.protoversion() >> 16) << "." << (packet.protoversion() & 0xFF);
		mHello = packet;

		if (mClientConnect)
		{
			// If we connected due to scan, no longer need to scan.
			theApp->getConnectionPool().peerVerified(shared_from_this());
		}

		if (!theApp->getConnectionPool().peerConnected(shared_from_this(), mNodePublic, getIP(), getPort()))
		{ // Already connected, self, or some other reason.
			cLog(lsINFO) << "Recv(Hello): Disconnect: Extraneous connection.";
		}
		else
		{
			if (mClientConnect)
			{
				// No longer connecting as client.
				mClientConnect	= false;
			}
			else
			{
				// Take a guess at remotes address.
				std::string	strIP	= getSocket().remote_endpoint().address().to_string();
				int			iPort	= packet.ipv4port();

				theApp->getConnectionPool().savePeer(strIP, iPort, UniqueNodeList::vsInbound);
			}

			// Consider us connected.  No longer accepting mtHELLO.
			mHelloed		= true;

			// XXX Set timer: connection is in grace period to be useful.
			// XXX Set timer: connection idle (idle may vary depending on connection type.)

			if ((packet.has_ledgerclosed()) && (packet.ledgerclosed().size() == (256 / 8)))
			{
				memcpy(mClosedLedgerHash.begin(), packet.ledgerclosed().data(), 256 / 8);
				if ((packet.has_ledgerprevious()) && (packet.ledgerprevious().size() == (256 / 8)))
				{
					memcpy(mPreviousLedgerHash.begin(), packet.ledgerprevious().data(), 256 / 8);
					addLedger(mPreviousLedgerHash);
				}
				else mPreviousLedgerHash.zero();
			}

			bDetach	= false;
		}
	}

	if (bDetach)
	{
		mNodePublic.clear();
		detach("recvh");
	}
	else
	{
		sendGetPeers();
	}
}

static void checkTransaction(Job&, int flags, SerializedTransaction::pointer stx, boost::weak_ptr<Peer> peer)
{

#ifndef TRUST_NETWORK
	try
	{
#endif
		Transaction::pointer tx;

		if ((flags & SF_SIGGOOD) != 0)
		{
			tx = boost::make_shared<Transaction>(stx, true);
			if (tx->getStatus() == INVALID)
			{
				theApp->getSuppression().setFlag(stx->getTransactionID(), SF_BAD);
				return;
			}
			else
				theApp->getSuppression().setFlag(stx->getTransactionID(), SF_SIGGOOD);
		}
		else
			tx = boost::make_shared<Transaction>(stx, false);

		theApp->getIOService().post(boost::bind(&NetworkOPs::processTransaction, &theApp->getOPs(), tx));

#ifndef TRUST_NETWORK
	}
	catch (...)
	{
		theApp->getSuppression().setFlags(stx->getTransactionID(), SF_BAD);
		punishPeer(peer, PP_INVALID_REQUEST);
	}
#endif
}

void Peer::recvTransaction(ripple::TMTransaction& packet)
{
	cLog(lsDEBUG) << "Got transaction from peer";

	Transaction::pointer tx;
#ifndef TRUST_NETWORK
	try
	{
#endif
		std::string rawTx = packet.rawtransaction();
		Serializer s(rawTx);
		SerializerIterator sit(s);
		SerializedTransaction::pointer stx = boost::make_shared<SerializedTransaction>(boost::ref(sit));

		int flags;
		if (!theApp->isNew(stx->getTransactionID(), mPeerId, flags))
		{ // we have seen this transaction recently
			if ((flags & SF_BAD) != 0)
			{
				punishPeer(PP_INVALID_REQUEST);
				return;
			}

			if ((flags & SF_RETRY) == 0)
				return;
		}

		theApp->getJobQueue().addJob(jtTRANSACTION,
			boost::bind(&checkTransaction, _1, flags, stx,  boost::weak_ptr<Peer>(shared_from_this())));

#ifndef TRUST_NETWORK
	}
	catch (...)
	{
#ifdef DEBUG
		std::cerr << "Transaction from peer fails validity tests" << std::endl;
		Json::StyledStreamWriter w;
		w.write(std::cerr, tx->getJson(0));
#endif
		return;
	}
#endif

}

void Peer::recvPropose(ripple::TMProposeSet& packet)
{
	if ((packet.currenttxhash().size() != 32) || (packet.nodepubkey().size() < 28) ||
		(packet.signature().size() < 56) || (packet.nodepubkey().size() > 128) || (packet.signature().size() > 128))
	{
		cLog(lsWARNING) << "Received proposal is malformed";
		return;
	}

	uint256 currentTxHash, prevLedger;
	memcpy(currentTxHash.begin(), packet.currenttxhash().data(), 32);

	if ((packet.has_previousledger()) && (packet.previousledger().size() == 32))
		memcpy(prevLedger.begin(), packet.previousledger().data(), 32);

	Serializer s(512);
	s.add256(currentTxHash);
	s.add256(prevLedger);
	s.add32(packet.proposeseq());
	s.add32(packet.closetime());
	s.addVL(packet.nodepubkey());
	s.addVL(packet.signature());
	uint256 suppression = s.getSHA512Half();

	if (!theApp->isNew(suppression, mPeerId))
		return;

	RippleAddress nodePublic = RippleAddress::createNodePublic(strCopy(packet.nodepubkey()));
//	bool isTrusted = theApp->getUNL().nodeInUNL(nodePublic);

	if(theApp->getOPs().recvPropose(suppression, packet.proposeseq(), currentTxHash, prevLedger, packet.closetime(),
		packet.signature(), nodePublic))
	{ // FIXME: Not all nodes will want proposals 
		PackedMessage::pointer message = boost::make_shared<PackedMessage>(packet, ripple::mtPROPOSE_LEDGER);
		theApp->getConnectionPool().relayMessage(this, message);
	}
}

void Peer::recvHaveTxSet(ripple::TMHaveTransactionSet& packet)
{
	// FIXME: We should have some limit on the number of HaveTxSet messages a peer can send us
	// per consensus pass, to keep a peer from running up our memory without limit
	uint256 hashes;
	if (packet.hash().size() != (256 / 8))
	{
		punishPeer(PP_INVALID_REQUEST);
		return;
	}
	uint256 hash;
	memcpy(hash.begin(), packet.hash().data(), 32);
	if (packet.status() == ripple::tsHAVE)
		addTxSet(hash);
	if (!theApp->getOPs().hasTXSet(shared_from_this(), hash, packet.status()))
		punishPeer(PP_UNWANTED_DATA);
}

static void checkValidation(Job&, SerializedValidation::pointer val, uint256 signingHash,
	bool isTrusted, boost::shared_ptr<ripple::TMValidation> packet, boost::weak_ptr<Peer> peer)
{
#ifndef TRUST_NETWORK
	try
#endif
	{
		if (!val->isValid(signingHash))
		{
			cLog(lsWARNING) << "Validation is invalid";
			Peer::punishPeer(peer, PP_UNKNOWN_REQUEST);
			return;
		}

		std::set<uint64> peers;
		if (theApp->getOPs().recvValidation(val) && theApp->getSuppression().swapSet(signingHash, peers, SF_RELAYED))
		{
			PackedMessage::pointer message = boost::make_shared<PackedMessage>(*packet, ripple::mtVALIDATION);
			theApp->getConnectionPool().relayMessageBut(peers, message);
		}
	}
#ifndef TRUST_NETWORK
	catch (...)
	{
		cLog(lsWARNING) << "Exception processing validation";
		Peer::punishPeer(peer, PP_UNKNOWN_REQUEST);
	}
#endif
}

void Peer::recvValidation(const boost::shared_ptr<ripple::TMValidation>& packet)
{
	if (packet->validation().size() < 50)
	{
		cLog(lsWARNING) << "Too small validation from peer";
		punishPeer(PP_UNKNOWN_REQUEST);
		return;
	}

#ifndef TRUST_NETWORK
	try
#endif
	{
		Serializer s(packet->validation());
		SerializerIterator sit(s);
		SerializedValidation::pointer val = boost::make_shared<SerializedValidation>(boost::ref(sit), false);

		uint256 signingHash = val->getSigningHash();
		if (!theApp->isNew(signingHash, mPeerId))
		{
			cLog(lsTRACE) << "Validation is duplicate";
			return;
		}

		bool isTrusted = theApp->getUNL().nodeInUNL(val->getSignerPublic());
		theApp->getJobQueue().addJob(isTrusted ? jtVALIDATION_t : jtVALIDATION_ut,
			boost::bind(&checkValidation, _1, val, signingHash, isTrusted, packet,
			boost::weak_ptr<Peer>(shared_from_this())));
	}
#ifndef TRUST_NETWORK
	catch (...)
	{
		cLog(lsWARNING) << "Exception processing validation";
		punishPeer(PP_UNKNOWN_REQUEST);
	}
#endif
}

void Peer::recvGetValidation(ripple::TMGetValidations& packet)
{
}

void Peer::recvContact(ripple::TMContact& packet)
{
}

void Peer::recvGetContacts(ripple::TMGetContacts& packet)
{
}

// return a list of your favorite people
// TODO: filter out all the LAN peers
// TODO: filter out the peer you are talking to
void Peer::recvGetPeers(ripple::TMGetPeers& packet)
{
	std::vector<std::string> addrs;

	theApp->getConnectionPool().getTopNAddrs(30, addrs);

	if (!addrs.empty())
	{
		ripple::TMPeers peers;

		for (unsigned int n=0; n<addrs.size(); n++)
		{
			std::string strIP;
			int			iPort;

			splitIpPort(addrs[n], strIP, iPort);

			// XXX This should also ipv6
			ripple::TMIPv4EndPoint* addr=peers.add_nodes();
			addr->set_ipv4(inet_addr(strIP.c_str()));
			addr->set_ipv4port(iPort);

			//cLog(lsINFO) << "Peer: Teaching: " << ADDRESS(this) << ": " << n << ": " << strIP << " " << iPort;
		}

		PackedMessage::pointer message = boost::make_shared<PackedMessage>(peers, ripple::mtPEERS);
		sendPacket(message);
	}
}

// TODO: filter out all the LAN peers
void Peer::recvPeers(ripple::TMPeers& packet)
{
	for (int i = 0; i < packet.nodes().size(); ++i)
	{
		in_addr addr;

		addr.s_addr	= packet.nodes(i).ipv4();

		std::string	strIP(inet_ntoa(addr));
		int			iPort	= packet.nodes(i).ipv4port();

		if (strIP != "0.0.0.0" && strIP != "127.0.0.1")
		{
			//cLog(lsINFO) << "Peer: Learning: " << ADDRESS(this) << ": " << i << ": " << strIP << " " << iPort;

			theApp->getConnectionPool().savePeer(strIP, iPort, UniqueNodeList::vsTold);
		}
	}
}

void Peer::recvGetObjectByHash(ripple::TMGetObjectByHash& packet)
{
	if (packet.query())
	{ // this is a query
		ripple::TMGetObjectByHash reply;

		reply.clear_query();
		if (packet.has_seq())
			reply.set_seq(packet.seq());
		reply.set_type(packet.type());
		if (packet.has_ledgerhash())
			reply.set_ledgerhash(packet.ledgerhash());

		// This is a very minimal implementation
		for (int i = 0; i < packet.objects_size(); ++i)
		{
			uint256 hash;
			const ripple::TMIndexedObject& obj = packet.objects(i);
			if (obj.has_hash() && (obj.hash().size() == (256/8)))
			{
				memcpy(hash.begin(), obj.hash().data(), 256 / 8);
				HashedObject::pointer hObj = theApp->getHashedObjectStore().retrieve(hash);
				if (hObj)
				{
					ripple::TMIndexedObject& newObj = *reply.add_objects();
					newObj.set_hash(hash.begin(), hash.size());
					newObj.set_data(&hObj->getData().front(), hObj->getData().size());
					if (obj.has_nodeid())
						newObj.set_index(obj.nodeid());
				}
			}
		}
		cLog(lsDEBUG) << "GetObjByHash query: had " << reply.objects_size() << " of " << packet.objects_size();
		sendPacket(boost::make_shared<PackedMessage>(packet, ripple::mtGET_OBJECTS));
	}
	else
	{ // this is a reply
		// WRITEME
	}
}

void Peer::recvPing(ripple::TMPing& packet)
{
}

void Peer::recvErrorMessage(ripple::TMErrorMsg& packet)
{
}

void Peer::recvSearchTransaction(ripple::TMSearchTransaction& packet)
{
}

void Peer::recvGetAccount(ripple::TMGetAccount& packet)
{
}

void Peer::recvAccount(ripple::TMAccount& packet)
{
}

void Peer::recvStatus(ripple::TMStatusChange& packet)
{
	cLog(lsTRACE) << "Received status change from peer " << getIP();
	if (!packet.has_networktime())
		packet.set_networktime(theApp->getOPs().getNetworkTimeNC());

	if (!mLastStatus.has_newstatus() || packet.has_newstatus())
		mLastStatus = packet;
	else
	{ // preserve old status
		ripple::NodeStatus status = mLastStatus.newstatus();
		mLastStatus = packet;
		packet.set_newstatus(status);
	}

	if (packet.newevent() == ripple::neLOST_SYNC)
	{
		if (!mClosedLedgerHash.isZero())
		{
			cLog(lsTRACE) << "peer has lost sync " << getIP();
			mClosedLedgerHash.zero();
		}
		mPreviousLedgerHash.zero();
		return;
	}
	if (packet.has_ledgerhash() && (packet.ledgerhash().size() == (256 / 8)))
	{ // a peer has changed ledgers
		memcpy(mClosedLedgerHash.begin(), packet.ledgerhash().data(), 256 / 8);
		addLedger(mClosedLedgerHash);
		cLog(lsTRACE) << "peer LCL is " << mClosedLedgerHash << " " << getIP();
	}
	else
	{
		cLog(lsTRACE) << "peer has no ledger hash" << getIP();
		mClosedLedgerHash.zero();
	}

	if (packet.has_ledgerhashprevious() && packet.ledgerhashprevious().size() == (256 / 8))
	{
		memcpy(mPreviousLedgerHash.begin(), packet.ledgerhashprevious().data(), 256 / 8);
		addLedger(mPreviousLedgerHash);
	}
	else mPreviousLedgerHash.zero();
}

void Peer::recvGetLedger(ripple::TMGetLedger& packet)
{
	SHAMap::pointer map;
	ripple::TMLedgerData reply;
	bool fatLeaves = true, fatRoot = false;

	if (packet.has_requestcookie())
		reply.set_requestcookie(packet.requestcookie());

	if (packet.itype() == ripple::liTS_CANDIDATE)
	{ // Request is  for a transaction candidate set
		cLog(lsINFO) << "Received request for TX candidate set data " << getIP();
		if ((!packet.has_ledgerhash() || packet.ledgerhash().size() != 32))
		{
			punishPeer(PP_INVALID_REQUEST);
			return;
		}
		uint256 txHash;
		memcpy(txHash.begin(), packet.ledgerhash().data(), 32);
		map = theApp->getOPs().getTXMap(txHash);
		if (!map)
		{
			cLog(lsERROR) << "We do not have the map our peer wants";
			punishPeer(PP_INVALID_REQUEST);
			return;
		}
		reply.set_ledgerseq(0);
		reply.set_ledgerhash(txHash.begin(), txHash.size());
		reply.set_type(ripple::liTS_CANDIDATE);
		fatLeaves = false; // We'll already have most transactions
		fatRoot = true; // Save a pass
	}
	else
	{ // Figure out what ledger they want
		cLog(lsINFO) << "Received request for ledger data " << getIP();
		Ledger::pointer ledger;
		if (packet.has_ledgerhash())
		{
			uint256 ledgerhash;
			if (packet.ledgerhash().size() != 32)
			{
				punishPeer(PP_INVALID_REQUEST);
				cLog(lsWARNING) << "Invalid request";
				return;
			}
			memcpy(ledgerhash.begin(), packet.ledgerhash().data(), 32);
			ledger = theApp->getMasterLedger().getLedgerByHash(ledgerhash);
			tLog(!ledger, lsINFO) << "Don't have ledger " << ledgerhash;
		}
		else if (packet.has_ledgerseq())
		{
			ledger = theApp->getMasterLedger().getLedgerBySeq(packet.ledgerseq());
			tLog(!ledger, lsINFO) << "Don't have ledger " << packet.ledgerseq();
		}
		else if (packet.has_ltype() && (packet.ltype() == ripple::ltCURRENT))
			ledger = theApp->getMasterLedger().getCurrentLedger();
		else if (packet.has_ltype() && (packet.ltype() == ripple::ltCLOSED) )
		{
			ledger = theApp->getMasterLedger().getClosedLedger();
			if (ledger && !ledger->isClosed())
				ledger = theApp->getMasterLedger().getLedgerBySeq(ledger->getLedgerSeq() - 1);
		}
		else
		{
			punishPeer(PP_INVALID_REQUEST);
			cLog(lsWARNING) << "Can't figure out what ledger they want";
			return;
		}

		if ((!ledger) || (packet.has_ledgerseq() && (packet.ledgerseq() != ledger->getLedgerSeq())))
		{
			punishPeer(PP_UNKNOWN_REQUEST);
			if (sLog(lsWARNING))
			{
				if (ledger)
					Log(lsWARNING) << "Ledger has wrong sequence";
				else
					Log(lsWARNING) << "Can't find the ledger they want";
			}
			return;
		}

		// Fill out the reply
		uint256 lHash = ledger->getHash();
		reply.set_ledgerhash(lHash.begin(), lHash.size());
		reply.set_ledgerseq(ledger->getLedgerSeq());
		reply.set_type(packet.itype());

		if(packet.itype() == ripple::liBASE)
		{ // they want the ledger base data
			cLog(lsTRACE) << "They want ledger base data";
			Serializer nData(128);
			ledger->addRaw(nData);
			reply.add_nodes()->set_nodedata(nData.getDataPtr(), nData.getLength());

			SHAMap::pointer map = ledger->peekAccountStateMap();
			if (map && map->getHash().isNonZero())
			{ // return account state root node if possible
				Serializer rootNode(768);
				if (map->getRootNode(rootNode, snfWIRE))
				{
					reply.add_nodes()->set_nodedata(rootNode.getDataPtr(), rootNode.getLength());
					if (ledger->getTransHash().isNonZero())
					{
						map = ledger->peekTransactionMap();
						if (map && map->getHash().isNonZero())
						{
							rootNode.erase();
							if (map->getRootNode(rootNode, snfWIRE))
								reply.add_nodes()->set_nodedata(rootNode.getDataPtr(), rootNode.getLength());
						}
					}
				}
			}

			PackedMessage::pointer oPacket = boost::make_shared<PackedMessage>(reply, ripple::mtLEDGER_DATA);
			sendPacket(oPacket);
			return;
		}

		if (packet.itype() == ripple::liTX_NODE)
			map = ledger->peekTransactionMap();
		else if (packet.itype() == ripple::liAS_NODE)
			map = ledger->peekAccountStateMap();
	}

	if ((!map) || (packet.nodeids_size() == 0))
	{
		cLog(lsWARNING) << "Can't find map or empty request";
		punishPeer(PP_INVALID_REQUEST);
		return;
	}

	for(int i = 0; i < packet.nodeids().size(); ++i)
	{
		SHAMapNode mn(packet.nodeids(i).data(), packet.nodeids(i).size());
		if(!mn.isValid())
		{
			punishPeer(PP_INVALID_REQUEST);
			return;
		}
		std::vector<SHAMapNode> nodeIDs;
		std::list< std::vector<unsigned char> > rawNodes;
		if(map->getNodeFat(mn, nodeIDs, rawNodes, fatRoot, fatLeaves))
		{
			std::vector<SHAMapNode>::iterator nodeIDIterator;
			std::list< std::vector<unsigned char> >::iterator rawNodeIterator;
			for(nodeIDIterator = nodeIDs.begin(), rawNodeIterator = rawNodes.begin();
				nodeIDIterator != nodeIDs.end(); ++nodeIDIterator, ++rawNodeIterator)
			{
				Serializer nID(33);
				nodeIDIterator->addIDRaw(nID);
				ripple::TMLedgerNode* node = reply.add_nodes();
				node->set_nodeid(nID.getDataPtr(), nID.getLength());
				node->set_nodedata(&rawNodeIterator->front(), rawNodeIterator->size());
			}
		}
	}
	PackedMessage::pointer oPacket = boost::make_shared<PackedMessage>(reply, ripple::mtLEDGER_DATA);
	sendPacket(oPacket);
}

void Peer::recvLedger(ripple::TMLedgerData& packet)
{
	if (packet.nodes().size() <= 0)
	{
		cLog(lsWARNING) << "Ledger data with no nodes";
		punishPeer(PP_INVALID_REQUEST);
		return;
	}

	if (packet.type() == ripple::liTS_CANDIDATE)
	{ // got data for a candidate transaction set
		uint256 hash;
		if(packet.ledgerhash().size() != 32)
		{
			punishPeer(PP_INVALID_REQUEST);
			return;
		}
		memcpy(hash.begin(), packet.ledgerhash().data(), 32);


		std::list<SHAMapNode> nodeIDs;
		std::list< std::vector<unsigned char> > nodeData;

		for (int i = 0; i < packet.nodes().size(); ++i)
		{
			const ripple::TMLedgerNode& node = packet.nodes(i);
			if (!node.has_nodeid() || !node.has_nodedata() || (node.nodeid().size() != 33))
			{
				cLog(lsWARNING) << "LedgerData request with invalid node ID";
				punishPeer(PP_INVALID_REQUEST);
				return;
			}
			nodeIDs.push_back(SHAMapNode(node.nodeid().data(), node.nodeid().size()));
			nodeData.push_back(std::vector<unsigned char>(node.nodedata().begin(), node.nodedata().end()));
		}
		if (!theApp->getOPs().gotTXData(shared_from_this(), hash, nodeIDs, nodeData))
			punishPeer(PP_UNWANTED_DATA);
		return;
	}

	if (!theApp->getMasterLedgerAcquire().gotLedgerData(packet, shared_from_this()))
		punishPeer(PP_UNWANTED_DATA);
}

bool Peer::hasLedger(const uint256& hash) const
{
	BOOST_FOREACH(const uint256& ledger, mRecentLedgers)
		if (ledger == hash)
			return true;
	return false;
}

void Peer::addLedger(const uint256& hash)
{
	BOOST_FOREACH(const uint256& ledger, mRecentLedgers)
		if (ledger == hash)
			return;
	if (mRecentLedgers.size() == 128)
		mRecentLedgers.pop_front();
	mRecentLedgers.push_back(hash);
}

bool Peer::hasTxSet(const uint256& hash) const
{
	BOOST_FOREACH(const uint256& set, mRecentTxSets)
		if (set == hash)
			return true;
	return false;
}

void Peer::addTxSet(const uint256& hash)
{
	BOOST_FOREACH(const uint256& set, mRecentTxSets)
		if (set == hash)
			return;
	if (mRecentTxSets.size() == 128)
		mRecentTxSets.pop_front();
	mRecentTxSets.push_back(hash);
}

// Get session information we can sign to prevent man in the middle attack.
// (both sides get the same information, neither side controls it)
void Peer::getSessionCookie(std::string& strDst)
{
	SSL* ssl = mSocketSsl.native_handle();
	if (!ssl) throw std::runtime_error("No underlying connection");

	// Get both finished messages
	unsigned char s1[1024], s2[1024];
	int l1 = SSL_get_finished(ssl, s1, sizeof(s1));
	int l2 = SSL_get_peer_finished(ssl, s2, sizeof(s2));

	if ((l1 < 12) || (l2 < 12))
		throw std::runtime_error(str(boost::format("Connection setup not complete: %d %d") % l1 % l2));

	// Hash them and XOR the results
	unsigned char sha1[64], sha2[64];

	SHA512(s1, l1, sha1);
	SHA512(s2, l2, sha2);
	if (memcmp(s1, s2, sizeof(sha1)) == 0)
		throw std::runtime_error("Identical finished messages");

	for (int i = 0; i < sizeof(sha1); ++i)
		sha1[i] ^= sha2[i];

	strDst.assign((char *) &sha1[0], sizeof(sha1));
}

void Peer::sendHello()
{
	std::string					strCookie;
	std::vector<unsigned char>	vchSig;

	getSessionCookie(strCookie);
	mCookieHash	= Serializer::getSHA512Half(strCookie);

	theApp->getWallet().getNodePrivate().signNodePrivate(mCookieHash, vchSig);

	ripple::TMHello h;

	h.set_protoversion(MAKE_VERSION_INT(PROTO_VERSION_MAJOR, PROTO_VERSION_MINOR));
	h.set_protoversionmin(MAKE_VERSION_INT(MIN_PROTO_MAJOR, MIN_PROTO_MINOR));
	h.set_fullversion(SERVER_VERSION);
	h.set_nettime(theApp->getOPs().getNetworkTimeNC());
	h.set_nodepublic(theApp->getWallet().getNodePublic().humanNodePublic());
	h.set_nodeproof(&vchSig[0], vchSig.size());
	h.set_ipv4port(theConfig.PEER_PORT);

	Ledger::pointer closedLedger = theApp->getMasterLedger().getClosedLedger();
	if (closedLedger && closedLedger->isClosed())
	{
		uint256 hash = closedLedger->getHash();
		h.set_ledgerclosed(hash.begin(), hash.GetSerializeSize());
		hash = closedLedger->getParentHash();
		h.set_ledgerprevious(hash.begin(), hash.GetSerializeSize());
	}

	PackedMessage::pointer packet = boost::make_shared<PackedMessage>(h, ripple::mtHELLO);
	sendPacket(packet);
}

void Peer::sendGetPeers()
{
	// get other peers this guy knows about
	ripple::TMGetPeers getPeers;

	getPeers.set_doweneedthis(1);

	PackedMessage::pointer packet = boost::make_shared<PackedMessage>(getPeers, ripple::mtGET_PEERS);

	sendPacket(packet);
}

void Peer::punishPeer(PeerPunish)
{
}

Json::Value Peer::getJson()
{
	Json::Value ret(Json::objectValue);

	//ret["this"]			= ADDRESS(this);
	ret["public_key"]	= mNodePublic.ToString();
	ret["ip"]			= mIpPortConnect.first;
	//ret["port"]			= mIpPortConnect.second;
	ret["port"]			= mIpPort.second;

	if (mHello.has_fullversion())
		ret["version"] = mHello.fullversion();

	if (mHello.has_protoversion() &&
			(mHello.protoversion() != MAKE_VERSION_INT(PROTO_VERSION_MAJOR, PROTO_VERSION_MINOR)))
		ret["protocol"] =  boost::lexical_cast<std::string>(GET_VERSION_MAJOR(mHello.protoversion())) + "." +
			boost::lexical_cast<std::string>(GET_VERSION_MINOR(mHello.protoversion()));

	if (!!mClosedLedgerHash)
		ret["ledger"] = mClosedLedgerHash.GetHex();

	if (mLastStatus.has_newstatus())
	{
		switch (mLastStatus.newstatus())
		{
			case ripple::nsCONNECTING:		ret["status"] = "connecting";	break;
			case ripple::nsCONNECTED:		ret["status"] = "connected";	break;
			case ripple::nsMONITORING:		ret["status"] = "monitoring";	break;
			case ripple::nsVALIDATING:		ret["status"] = "validating";	break;
			case ripple::nsSHUTTING:		ret["status"] = "shutting";		break;
			default:						cLog(lsWARNING) << "Peer has unknown status: " << mLastStatus.newstatus();
		}
	}

	/*
	if (!mIpPort.first.empty())
	{
		ret["verified_ip"]		= mIpPort.first;
		ret["verified_port"]	= mIpPort.second;
	}*/

	return ret;
}

// vim:ts=4
