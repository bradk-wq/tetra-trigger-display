#pragma once

// Minimal POSIX UDP helpers shared by the receiver and sender.

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace tetra::wireless {

struct Endpoint {
    sockaddr_storage address{};
    socklen_t length = 0;
};

class UdpSocket {
public:
    UdpSocket() {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) {
            throw std::runtime_error("socket() failed");
        }
        // Ask for a deep receive buffer: a 20 KB frame is 17 datagrams that
        // arrive back to back.
        int bytes = 1 << 20;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
        // DSCP EF. Wi-Fi maps this to the voice access category (WMM), which
        // is the lowest-latency queue; wired networks ignore it.
        int tos = 0xb8;
        ::setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    }
    ~UdpSocket() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    void bindPort(std::uint16_t port) {
        int yes = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address))
            != 0) {
            throw std::runtime_error("bind() failed on port "
                                     + std::to_string(port));
        }
    }

    void setReceiveTimeoutMs(int milliseconds) {
        timeval tv{};
        tv.tv_sec = milliseconds / 1000;
        tv.tv_usec = (milliseconds % 1000) * 1000;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /// Returns bytes received, or -1 on timeout or error.
    long receive(void *buffer, std::size_t size, Endpoint &from) {
        from.length = sizeof(from.address);
        return ::recvfrom(fd_, buffer, size, 0,
                          reinterpret_cast<sockaddr *>(&from.address),
                          &from.length);
    }

    bool send(const void *data, std::size_t size, const Endpoint &to) {
        return ::sendto(fd_, data, size, 0,
                        reinterpret_cast<const sockaddr *>(&to.address),
                        to.length)
               == static_cast<long>(size);
    }

private:
    int fd_ = -1;
};

inline Endpoint resolve(const std::string &hostAndPort,
                        std::uint16_t defaultPort) {
    std::string host = hostAndPort;
    std::string port = std::to_string(defaultPort);
    const auto colon = hostAndPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostAndPort.substr(0, colon);
        port = hostAndPort.substr(colon + 1);
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *found = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0
        || found == nullptr) {
        throw std::runtime_error("cannot resolve " + hostAndPort);
    }
    Endpoint endpoint;
    std::memcpy(&endpoint.address, found->ai_addr, found->ai_addrlen);
    endpoint.length = found->ai_addrlen;
    ::freeaddrinfo(found);
    return endpoint;
}

}
