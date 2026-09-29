// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <aperture/rpc_client.h>

#include <util/strencodings.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace aperture {
namespace {

class FdGuard
{
public:
    explicit FdGuard(int fd) : m_fd(fd) {}
    ~FdGuard()
    {
        if (m_fd >= 0) close(m_fd);
    }
    int get() const { return m_fd; }

private:
    int m_fd;
};

int ConnectTcp(const std::string& host, uint16_t port, int timeout_seconds)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res{nullptr};
    const std::string port_str{std::to_string(port)};
    if (int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res); rc != 0) {
        throw RpcTransportError(std::string("getaddrinfo: ") + gai_strerror(rc));
    }
    int fd{-1};
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        timeval tv{};
        tv.tv_sec = timeout_seconds;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) throw RpcTransportError("cannot connect to " + host + ":" + port_str);
    return fd;
}

void SendAll(int fd, const std::string& data)
{
    size_t sent{0};
    while (sent < data.size()) {
        const ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            throw RpcTransportError(std::string("send: ") + std::strerror(errno));
        }
        sent += static_cast<size_t>(n);
    }
}

std::string RecvAll(int fd)
{
    std::string out;
    char buf[65536];
    while (true) {
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            throw RpcTransportError(std::string("recv: ") + std::strerror(errno));
        }
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

} // namespace

RpcClient::RpcClient(std::string host, uint16_t port, std::string user, std::string password)
    : m_host(std::move(host)), m_port(port), m_auth(EncodeBase64(user + ":" + password)) {}

RpcClient RpcClient::FromCookie(std::string host, uint16_t port, const std::string& cookie_path)
{
    std::ifstream file(cookie_path);
    std::string cookie;
    if (!file || !std::getline(file, cookie)) {
        throw RpcTransportError("cannot read RPC cookie file " + cookie_path);
    }
    const auto colon = cookie.find(':');
    if (colon == std::string::npos) throw RpcTransportError("malformed RPC cookie file " + cookie_path);
    return RpcClient(std::move(host), port, cookie.substr(0, colon), cookie.substr(colon + 1));
}

UniValue RpcClient::Call(const std::string& method, const UniValue& params, int timeout_seconds) const
{
    UniValue request(UniValue::VOBJ);
    request.pushKV("jsonrpc", "1.0");
    request.pushKV("id", "sv2-tp");
    request.pushKV("method", method);
    request.pushKV("params", params);
    const std::string body{request.write()};

    std::ostringstream http;
    http << "POST / HTTP/1.1\r\n"
         << "Host: " << m_host << "\r\n"
         << "Connection: close\r\n"
         << "Authorization: Basic " << m_auth << "\r\n"
         << "Content-Type: application/json\r\n"
         << "Content-Length: " << body.size() << "\r\n\r\n"
         << body;

    FdGuard fd{ConnectTcp(m_host, m_port, timeout_seconds)};
    SendAll(fd.get(), http.str());
    const std::string response{RecvAll(fd.get())};

    const auto header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) throw RpcTransportError("malformed HTTP response");
    const std::string status_line{response.substr(0, response.find("\r\n"))};
    if (status_line.find(" 401") != std::string::npos) throw RpcTransportError("RPC authentication failed");

    UniValue reply;
    if (!reply.read(response.substr(header_end + 4)) || !reply.isObject()) {
        throw RpcTransportError("invalid JSON-RPC reply (" + status_line + ")");
    }
    const UniValue error = find_value(reply, "error");
    if (!error.isNull()) {
        const UniValue code = find_value(error, "code");
        const UniValue message = find_value(error, "message");
        throw RpcError(code.isNum() ? code.get_int() : -1, message.isStr() ? message.get_str() : error.write());
    }
    return find_value(reply, "result");
}

} // namespace aperture
