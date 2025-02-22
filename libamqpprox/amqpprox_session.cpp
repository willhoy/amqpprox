/*
** Copyright 2020 Bloomberg Finance L.P.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
#include <amqpprox_session.h>

#include <amqpprox_backend.h>
#include <amqpprox_bufferhandle.h>
#include <amqpprox_bufferpool.h>
#include <amqpprox_connectionmanager.h>
#include <amqpprox_connectionselector.h>
#include <amqpprox_constants.h>
#include <amqpprox_dnsresolver.h>
#include <amqpprox_eventsource.h>
#include <amqpprox_flowtype.h>
#include <amqpprox_frame.h>
#include <amqpprox_logging.h>
#include <amqpprox_method.h>
#include <amqpprox_methods_start.h>
#include <amqpprox_methods_startok.h>
#include <amqpprox_packetprocessor.h>
#include <amqpprox_proxyprotocolheaderv1.h>
#include <amqpprox_tlsutil.h>

#include <boost/system/error_code.hpp>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace Bloomberg {
namespace amqpprox {

// Implementation notes:
//
// TCP Settings:
//  - Non-blocking sockets: this is to support the edge notification, then
//    querying the amount that can be read without blocking.
//  - No delay: this is used to disable nagling, so that it doesn't
//    artificially slow down transit of messages to clients with low throughput
//  - We currently do not attempt to tune TCP receive buffer sizes, as we want
//    to get the per connection adaption based on the heavyness of the
//    connection being proxied.
//
// Ingress/Egress direction:
//
// Ingress in this component means that data has originated at the client and
// is being sent to the broker, egress means that it's data coming from the
// broker and being sent through the proxy to the client.

using namespace boost::asio::ip;
using namespace boost::system;

Session::Session(boost::asio::io_service &              ioservice,
                 MaybeSecureSocketAdaptor &&            serverSocket,
                 MaybeSecureSocketAdaptor &&            clientSocket,
                 ConnectionSelector *                   connectionSelector,
                 EventSource *                          eventSource,
                 BufferPool *                           bufferPool,
                 DNSResolver *                          dnsResolver,
                 const std::shared_ptr<HostnameMapper> &hostnameMapper,
                 std::string_view                       localHostname)
: d_ioService(ioservice)
, d_serverSocket(std::move(serverSocket))
, d_clientSocket(std::move(clientSocket))
, d_serverDataHandle()
, d_serverWriteDataHandle()
, d_clientDataHandle()
, d_clientWriteDataHandle()
, d_serverWaterMark(0)
, d_clientWaterMark(0)
, d_sessionState(hostnameMapper)
, d_connector(&d_sessionState, eventSource, bufferPool, localHostname)
, d_connectionSelector_p(connectionSelector)
, d_eventSource_p(eventSource)
, d_bufferPool_p(bufferPool)
, d_dnsResolver_p(dnsResolver)
, d_ingressWaitingSince()
, d_egressWaitingSince()
, d_egressRetryCounter(0)
, d_ingressCurrentlyReading(false)
, d_ingressStartedAt()
, d_egressCurrentlyReading(false)
, d_egressStartedAt()
, d_resolvedEndpoints()
, d_resolvedEndpointsIndex(0)
{
    boost::system::error_code ec;
    d_serverSocket.setDefaultOptions(ec);

    if (ec) {
        LOG_ERROR << "Setting options onto listening socket failed with: "
                  << ec;
    }
}

Session::~Session()
{
}

bool Session::finished()
{
    return d_sessionState.getDisconnectType() !=
           SessionState::DisconnectType::NOT_DISCONNECTED;
}

void Session::start()
{
    boost::system::error_code ecl, ecr;
    d_sessionState.setIngress(d_ioService,
                              d_serverSocket.local_endpoint(ecl),
                              d_serverSocket.remote_endpoint(ecr));

    if (ecl || ecr) {
        LOG_WARN << "Failed to get ingress socket endpoints: local=" << ecl
                 << ", remote=" << ecr
                 << " continuing to try to handshake anyway";
    }

    auto creationHandler = [this] { establishConnection(); };
    d_connector.setConnectionCreationHandler(creationHandler);

    auto self(shared_from_this());
    auto handshake_cb = [this, self](const error_code &ec) {
        if (ec) {
            handleSessionError("ssl", FlowType::INGRESS, ec);
            return;
        }

        // Expect data from the clients first
        readData(FlowType::INGRESS);
    };
    d_serverSocket.async_handshake(boost::asio::ssl::stream_base::server,
                                   handshake_cb);
}

void Session::attemptConnection(
    const std::shared_ptr<ConnectionManager> &connectionManager)
{
    if (d_sessionState.getPaused()) {
        LOG_DEBUG << "Not establishing a connection because paused";
        return;
    }

    if (finished()) {
        LOG_WARN << "Not establishing a connection because client already "
                 << " disconnected";
        return;
    }

    const Backend *backend =
        connectionManager->getConnection(d_egressRetryCounter);

    if (backend == nullptr) {
        LOG_ERROR << "attemptConnection: No backends available for connection,"
                  << " on retry: " << d_egressRetryCounter;
        disconnect(true);
        // TODO notify EventSource
        return;
    }

    using endpointType = boost::asio::ip::tcp::endpoint;
    auto self(shared_from_this());
    auto callback = [this, self, connectionManager](
                        const error_code &        ec,
                        std::vector<endpointType> endpoints) {
        BOOST_LOG_SCOPED_THREAD_ATTR(
            "Vhost",
            boost::log::attributes::constant<std::string>(
                d_sessionState.getVirtualHost()));
        BOOST_LOG_SCOPED_THREAD_ATTR(
            "ConnID",
            boost::log::attributes::constant<uint64_t>(d_sessionState.id()));

        auto currentBackend =
            connectionManager->getConnection(d_egressRetryCounter);

        // With Boost ASIO it sometimes on Linux returns a good error code,
        // but no items in the list. This catches this case as well as the
        // regular error return.
        if (!ec && !endpoints.empty()) {
            if (currentBackend && currentBackend->dnsBasedEntry()) {
                d_resolvedEndpoints = endpoints;
            }
            else {
                d_resolvedEndpoints.resize(0);
                d_resolvedEndpoints.push_back(endpoints[0]);
            }

            d_resolvedEndpointsIndex = 0;
            attemptResolvedConnection(connectionManager);
        }
        else {
            // Get the backend we tried for its name before incrementing
            // the retry counter

            if (currentBackend) {
                LOG_ERROR << "Failed to resolve " << currentBackend->host()
                          << ":" << currentBackend->port()
                          << " error_code: " << ec << " for "
                          << currentBackend->name();
            }
            else {
                LOG_ERROR << "Failed to resolve non-existing backend";
            }

            d_egressRetryCounter++;
            attemptConnection(connectionManager);
        }
    };

    if (backend->dnsBasedEntry()) {
        d_dnsResolver_p->resolve(
            backend->host(), std::to_string(backend->port()), callback);
    }
    else {
        // If this isn't a DNS based backend we still use the resolver, but
        // with the pre-cached at creation time IP address
        d_dnsResolver_p->resolve(
            backend->ip(), std::to_string(backend->port()), callback);
    }
}

void Session::attemptResolvedConnection(
    const std::shared_ptr<ConnectionManager> &connectionManager)
{
    if (d_resolvedEndpoints.empty() ||
        d_resolvedEndpointsIndex >= d_resolvedEndpoints.size()) {
        // If we've run out of endpoints or the returned set was empty we must
        // try the next backend
        d_resolvedEndpoints.resize(0);
        d_resolvedEndpointsIndex = 0;
        d_egressRetryCounter++;
        LOG_TRACE << "Run out of items on backend, moving onto next backend";
        attemptConnection(connectionManager);
    }
    else {
        auto index    = d_resolvedEndpointsIndex++;
        auto endpoint = d_resolvedEndpoints[index];
        LOG_TRACE << "Try index " << index << " of backend resolutions ("
                  << endpoint << ")";
        attemptEndpointConnection(endpoint, connectionManager);
    }
}

void Session::attemptEndpointConnection(
    boost::asio::ip::tcp::endpoint            endpoint,
    const std::shared_ptr<ConnectionManager> &connectionManager)
{
    auto self(shared_from_this());
    d_clientSocket.async_connect(
        endpoint, [this, self, connectionManager](error_code ec) {
            BOOST_LOG_SCOPED_THREAD_ATTR(
                "Vhost",
                boost::log::attributes::constant<std::string>(
                    d_sessionState.getVirtualHost()));
            BOOST_LOG_SCOPED_THREAD_ATTR(
                "ConnID",
                boost::log::attributes::constant<uint64_t>(
                    d_sessionState.id()));

            if (ec) {
                handleConnectionError("async_connect", ec, connectionManager);
                return;
            }

            auto local_endpoint = d_clientSocket.local_endpoint(ec);
            if (ec) {
                handleConnectionError("local_endpoint", ec, connectionManager);
                return;
            }

            auto remote_endpoint = d_clientSocket.remote_endpoint(ec);
            if (ec) {
                handleConnectionError(
                    "remote_endpoint", ec, connectionManager);
                return;
            }

            d_sessionState.setEgress(
                d_ioService, local_endpoint, remote_endpoint);

            d_clientSocket.setDefaultOptions(ec);
            if (ec) {
                handleConnectionError(
                    "setDefaultOptions", ec, connectionManager);
                return;
            }

            // Get the current backend and the remote client
            auto currentBackend =
                connectionManager->getConnection(d_egressRetryCounter);

            d_clientSocket.setSecure(currentBackend->tlsEnabled());

            LOG_INFO << "Starting "
                     << (currentBackend->tlsEnabled() ? "secured " : "")
                     << "connection for: " << d_sessionState;

            auto self(shared_from_this());
            auto handshake_cb = [this, self, connectionManager](
                                    const error_code &ec) {
                BOOST_LOG_SCOPED_THREAD_ATTR(
                    "Vhost",
                    boost::log::attributes::constant<std::string>(
                        d_sessionState.getVirtualHost()));
                BOOST_LOG_SCOPED_THREAD_ATTR(
                    "ConnID",
                    boost::log::attributes::constant<uint64_t>(
                        d_sessionState.id()));

                if (ec) {
                    handleSessionError("ssl", FlowType::INGRESS, ec);
                    return;
                }

                LOG_TRACE << "Post-handshake sending protocol header for:"
                          << d_sessionState;

                d_connector.synthesizeProtocolHeader();
                handleWriteData(
                    FlowType::EGRESS, d_clientSocket, d_connector.outBuffer());
            };

            if (!currentBackend->proxyProtocolEnabled()) {
                d_clientSocket.async_handshake(
                    boost::asio::ssl::stream_base::client, handshake_cb);
            }
            else {
                d_connector.synthesizeProxyProtocolHeader(
                    getProxyProtocolHeader(currentBackend));
                Buffer data = d_connector.outBuffer();

                LOG_TRACE << "Sending proxy protocol header ahead of any TLS "
                             "handshaking";

                auto writeHandler =
                    [this, self, hscb{std::move(handshake_cb)}](error_code ec,
                                                                std::size_t) {
                        if (ec) {
                            handleSessionError("write", FlowType::INGRESS, ec);
                            return;
                        }

                        d_clientSocket.async_handshake(
                            boost::asio::ssl::stream_base::client, hscb);
                    };

                boost::asio::async_write(
                    d_clientSocket,
                    boost::asio::buffer(data.ptr(), data.available()),
                    writeHandler);
            }
        });
}

std::string Session::getProxyProtocolHeader(const Backend *currentBackend)
{
    // For now only Proxy Protocol V1 is supported
    LOG_INFO << "Proxy Protocol V1 is enabled for: " << d_sessionState;
    auto              remoteClient = d_sessionState.getIngress().second;
    std::stringstream remoteClientAddress;
    remoteClientAddress << remoteClient.address();

    // Add Proxy Protocol-V1 Header
    ProxyProtocolHeaderV1 header =
        ProxyProtocolHeaderV1(ProxyProtocolHeaderV1::InetProtocol::TCP4,
                              remoteClientAddress.str(),
                              currentBackend->ip(),
                              remoteClient.port(),
                              currentBackend->port());

    std::stringstream proxyProtocolHeaderStream;
    proxyProtocolHeaderStream << header;
    return proxyProtocolHeaderStream.str();
}

void Session::establishConnection()
{
    if (d_sessionState.getPaused()) {
        LOG_DEBUG << "Not establishing a connection because paused";
        return;
    }

    std::shared_ptr<ConnectionManager> connectionManager;
    int rc = d_connectionSelector_p->acquireConnection(&connectionManager,
                                                       d_sessionState);
    if (0 != rc) {
        // Failure reason logged within acquireConnection
        disconnect(true);
        return;
    }

    attemptConnection(connectionManager);
}

void Session::print(std::ostream &os)
{
    TimePoint now = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> ingressTime =
        now - timePoint(FlowType::INGRESS);
    std::chrono::duration<double> egressTime =
        now - timePoint(FlowType::EGRESS);
    os << d_sessionState << " " << ingressTime.count() << ":"
       << egressTime.count() << std::endl;
}

void Session::pause()
{
    if (!d_sessionState.getPaused()) {
        d_sessionState.setPaused(true);
    }
}

void Session::disconnect(bool forcible)
{
    if (forcible) {
        auto self(shared_from_this());
        d_sessionState.setDisconnected(
            SessionState::DisconnectType::DISCONNECTED_PROXY);
        d_ioService.dispatch([this, self] {
            BOOST_LOG_SCOPED_THREAD_ATTR(
                "Vhost",
                boost::log::attributes::constant<std::string>(
                    d_sessionState.getVirtualHost()));
            BOOST_LOG_SCOPED_THREAD_ATTR(
                "ConnID",
                boost::log::attributes::constant<uint64_t>(
                    d_sessionState.id()));

            performDisconnectBoth();
        });
    }
    else {
        d_connector.synthesizeClose(true);
        sendSyntheticData();
    }
}

void Session::backendDisconnect()
{
    auto self(shared_from_this());
    d_ioService.dispatch([this, self] {
        BOOST_LOG_SCOPED_THREAD_ATTR(
            "Vhost",
            boost::log::attributes::constant<std::string>(
                d_sessionState.getVirtualHost()));
        BOOST_LOG_SCOPED_THREAD_ATTR(
            "ConnID",
            boost::log::attributes::constant<uint64_t>(d_sessionState.id()));

        boost::system::error_code shutdownEc;
        d_clientSocket.shutdown(shutdownEc);

        if (shutdownEc) {
            LOG_WARN << "Backend Disconnect shutdown failed rc: "
                     << shutdownEc;
            // Fall through: we still want to attempt to close the socket
        }

        boost::system::error_code closeEc;
        d_clientSocket.close(closeEc);

        if (closeEc) {
            LOG_WARN << "Backend Disconnect close failed rc: " << closeEc;
        }
    });
}

void Session::handleWriteData(FlowType                  direction,
                              MaybeSecureSocketAdaptor &writeSocket,
                              Buffer                    data)
{
    auto self(shared_from_this());
    auto writeHandler = [this, self, direction](error_code ec, std::size_t) {
        BOOST_LOG_SCOPED_THREAD_ATTR(
            "Vhost",
            boost::log::attributes::constant<std::string>(
                d_sessionState.getVirtualHost()));
        BOOST_LOG_SCOPED_THREAD_ATTR(
            "ConnID",
            boost::log::attributes::constant<uint64_t>(d_sessionState.id()));

        auto &watermark = waterMark(direction);
        if (0 == watermark) {
            bufferHandle(direction).release();
            bufferWriteDataHandle(direction).release();
        }

        if (ec) {
            handleSessionError("write", direction, ec);
            return;
        }

        uint64_t latency =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                TimePoint() - startedAt(direction))
                .count();

        if (direction == FlowType::INGRESS) {
            d_sessionState.addIngressLatency(latency);
        }
        else {
            d_sessionState.addEgressLatency(latency);
        }
        currentlyReading(direction) = false;

        readData(direction);
    };

    LOG_TRACE << "Write of " << data.available() << " bytes";
    boost::asio::async_write(writeSocket,
                             boost::asio::buffer(data.ptr(), data.available()),
                             writeHandler);
}

void Session::readData(FlowType direction)
{
    auto &socket         = readSocket(direction);
    timePoint(direction) = TimePoint();

    auto self(shared_from_this());
    socket.async_read_some(
        boost::asio::null_buffers(),
        [this, self, direction](error_code ec, std::size_t) {
            BOOST_LOG_SCOPED_THREAD_ATTR(
                "Vhost",
                boost::log::attributes::constant<std::string>(
                    d_sessionState.getVirtualHost()));
            BOOST_LOG_SCOPED_THREAD_ATTR(
                "ConnID",
                boost::log::attributes::constant<uint64_t>(
                    d_sessionState.id()));

            auto &socket         = readSocket(direction);
            timePoint(direction) = TimePoint();
            if (!currentlyReading(direction)) {
                startedAt(direction)        = TimePoint();
                currentlyReading(direction) = true;
            }

            if (ec) {
                handleSessionError("read", direction, ec);
                return;
            }

            std::size_t available = socket.available(ec);

            if (ec) {
                handleSessionError("socket-available", direction, ec);
                return;
            }

            // NB: This seems like a weird thing to do.
            //
            // Unfortunately some versions of boost ASIO optimize away 0
            // byte reads, and do not send the EOF through the error
            // code on available() or from the edge notification of
            // async_read_some(). This leaves us with a good error_code
            // and a 0 byte read, which is skipped as good by the
            // read_some(). To work around this, to receive the EOF in
            // the read_some(), we always make sure we ask for a 1 byte
            // read and we skip the `would_block` error code, and get
            // that case to go through the edge transition again.
            available = std::max(available, 1ul);

            auto &bufh      = bufferHandle(direction);
            auto &watermark = waterMark(direction);

            // If there's data in the buffer we shouldn't reallocate a new one
            // for this connection
            if (0 == watermark) {
                d_bufferPool_p->acquireBuffer(&bufh, available);
            }

            Buffer      readBuf    = readBuffer(direction);
            std::size_t readAmount = socket.read_some(
                boost::asio::buffer(readBuf.ptr(), readBuf.available()), ec);

            if (!ec) {
                watermark += readAmount;
                if (direction == FlowType::EGRESS ||
                    !d_sessionState.getPaused()) {
                    handleData(direction);
                }
            }
            else if (ec == boost::asio::error::would_block) {
                readData(direction);
            }
            else {
                handleSessionError("read_some", direction, ec);
                return;
            }
        });
}

void Session::sendSyntheticData()
{
    const Buffer outBuffer = d_connector.outBuffer();
    if (outBuffer.size()) {
        auto &writeSocket =
            d_connector.sendToIngressSide() ? d_serverSocket : d_clientSocket;
        handleWriteData(FlowType::EGRESS, writeSocket, outBuffer);
    }

    if (d_connector.state() == Connector::State::ERROR) {
        disconnect(true);
    }
}

void Session::handleData(FlowType direction)
{
    try {
        Buffer          readBuf = readBuffer(direction);
        PacketProcessor processor(d_sessionState, d_connector);
        processor.process(direction, readBuf);

        Buffer remaining = processor.remaining();
        copyRemaining(direction, remaining);

        Buffer ingressWrite = processor.ingressWrite();
        Buffer egressWrite  = processor.egressWrite();

        bool isOpen = d_connector.state() == Connector::State::OPEN;
        if (ingressWrite.size()) {
            handleWriteData(isOpen ? FlowType::EGRESS : FlowType::INGRESS,
                            d_serverSocket,
                            ingressWrite);
        }
        else if (egressWrite.size()) {
            handleWriteData(isOpen ? FlowType::INGRESS : FlowType::EGRESS,
                            d_clientSocket,
                            egressWrite);
        }
        else {
            readData(direction);
        }

        if (d_connector.state() == Connector::State::CLOSED) {
            performDisconnectBoth();
        }

        if (d_connector.state() == Connector::State::ERROR) {
            disconnect(true);
        }
    }
    catch (std::runtime_error &error) {
        LOG_ERROR << "Received exception: " << error.what() << " conn="
                  << d_sessionState.hostname(
                         d_sessionState.getIngress().second)
                  << ":" << d_sessionState.getIngress().second.port() << "->"
                  << d_sessionState.hostname(d_sessionState.getEgress().second)
                  << ":" << d_sessionState.getEgress().second.port()
                  << " direction=" << direction;

        disconnect(true);
    }
}

void Session::handleSessionError(const char *              action,
                                 FlowType                  direction,
                                 boost::system::error_code ec)
{
    if (d_connector.state() == Connector::State::CLOSED) {
        d_sessionState.setDisconnected(
            SessionState::DisconnectType::DISCONNECTED_CLEANLY);
    }
    else if (direction == FlowType::INGRESS &&
             d_connector.state() == Connector::State::CLIENT_CLOSE_SENT) {
        // The client might just close the socket or cancel read without
        // sending CloseOk back.
        LOG_WARN << "Failed to receive CloseOk from the client. "
                    "Sending Close to server. "
                    "Action:"
                 << action << " received error_code=" << ec << " "
                 << TlsUtil::augmentTlsError(ec) << " conn="
                 << d_sessionState.hostname(d_sessionState.getIngress().second)
                 << ":" << d_sessionState.getIngress().second.port() << "->"
                 << d_sessionState.hostname(d_sessionState.getEgress().second)
                 << ":" << d_sessionState.getEgress().second.port()
                 << " direction=" << direction;

        d_connector.synthesizeClose(false);
        sendSyntheticData();  // sending Close to server side

        d_connector.synthesizeCloseError(true);
        sendSyntheticData();  // sending Close error to client side
        return;
    }
    else if (d_sessionState.getDisconnectType() ==
             SessionState::DisconnectType::NOT_DISCONNECTED) {
        if (direction == FlowType::INGRESS) {
            d_sessionState.setDisconnected(
                SessionState::DisconnectType::DISCONNECTED_CLIENT);
        }
        else {
            d_sessionState.setDisconnected(
                SessionState::DisconnectType::DISCONNECTED_SERVER);
        }
    }

    if (ec != boost::asio::error::operation_aborted) {
        // TODO notify EventSource
        LOG_WARN << action << " received error_code=" << ec << " "
                 << TlsUtil::augmentTlsError(ec) << " conn="
                 << d_sessionState.hostname(d_sessionState.getIngress().second)
                 << ":" << d_sessionState.getIngress().second.port() << "->"
                 << d_sessionState.hostname(d_sessionState.getEgress().second)
                 << ":" << d_sessionState.getEgress().second.port()
                 << " direction=" << direction;

        boost::system::error_code clientCloseEc, serverCloseEc;
        d_clientSocket.close(clientCloseEc);
        d_serverSocket.close(serverCloseEc);

        if (clientCloseEc || serverCloseEc) {
            LOG_WARN << "Socket close failed: client=" << clientCloseEc
                     << ", server=" << serverCloseEc;
        }
    }
}

void Session::handleConnectionError(
    const char *                              action,
    boost::system::error_code                 ec,
    const std::shared_ptr<ConnectionManager> &connectionManager)
{
    std::string name;
    auto backend = connectionManager->getConnection(d_egressRetryCounter);
    if (backend) {
        name = backend->name();
    }

    LOG_WARN << action << " received connecting to '" << name
             << "' error_code=" << TlsUtil::augmentTlsError(ec) << " conn="
             << d_sessionState.hostname(d_sessionState.getIngress().second)
             << ":" << d_sessionState.getIngress().second.port() << "->"
             << d_sessionState.hostname(d_sessionState.getEgress().second)
             << ":" << d_sessionState.getIngress().second.port();

    attemptResolvedConnection(connectionManager);
}

void Session::performDisconnectBoth()
{
    boost::system::error_code clientShutdownEc, serverShutdownEc;
    d_clientSocket.shutdown(clientShutdownEc);
    d_serverSocket.shutdown(serverShutdownEc);

    if (clientShutdownEc || serverShutdownEc) {
        LOG_WARN << "Socket shutdown failed: client=" << clientShutdownEc
                 << ", server=" << serverShutdownEc
                 << ", continuing to close anyway";
    }

    boost::system::error_code clientCloseEc, serverCloseEc;
    d_clientSocket.close(clientCloseEc);
    d_serverSocket.close(serverCloseEc);

    if (clientCloseEc || serverCloseEc) {
        LOG_WARN << "Socket close failed: client=" << clientCloseEc
                 << ", server=" << serverCloseEc;
    }
}

boost::asio::io_service &Session::ioService()
{
    return d_ioService;
}

}
}
