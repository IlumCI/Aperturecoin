// Copyright (c) 2026 The ApertureCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef APERTURE_RPC_CLIENT_H
#define APERTURE_RPC_CLIENT_H

#include <univalue.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace aperture {

/** Error returned by the node (JSON-RPC "error" member). */
class RpcError : public std::runtime_error
{
public:
    RpcError(int code, const std::string& message) : std::runtime_error(message), m_code(code) {}
    int Code() const { return m_code; }

private:
    int m_code;
};

/** Transport-level failure (connection refused, HTTP error, bad JSON). */
class RpcTransportError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/**
 * Minimal blocking JSON-RPC 1.0 client for apertured (HTTP/1.1 over TCP,
 * Basic authentication). One connection per call; thread-safe.
 */
class RpcClient
{
public:
    RpcClient(std::string host, uint16_t port, std::string user, std::string password);

    /** Read credentials from a "user:password" cookie file (apertured .cookie). */
    static RpcClient FromCookie(std::string host, uint16_t port, const std::string& cookie_path);

    /** Call method with positional params; returns the "result" member. */
    UniValue Call(const std::string& method, const UniValue& params = UniValue(UniValue::VARR),
                  int timeout_seconds = 60) const;

private:
    std::string m_host;
    uint16_t m_port;
    std::string m_auth; //!< base64("user:password")
};

} // namespace aperture

#endif // APERTURE_RPC_CLIENT_H
