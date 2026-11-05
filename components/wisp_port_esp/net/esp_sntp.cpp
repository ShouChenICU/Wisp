#include "wisp/port/esp/esp_sntp.hpp"

#include <sys/time.h>

#include <cerrno>
#include <cstring>
#include <ctime>

#include "esp_random.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "wisp/base/error.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/esp/sntp_packet.hpp"

namespace wisp::port::esp {

namespace {

// Closes the socket on every path out of query().
struct SocketGuard {
    int fd = -1;
    ~SocketGuard() {
        if (fd >= 0) lwip_close(fd);
    }
};

Result<int64_t> io_error(const char* what) {
    return Result<int64_t>::err(ErrorCode::IoError,
                                std::string(what) + ": errno " + std::to_string(errno));
}

}  // namespace

Result<int64_t> EspSntp::query(const std::string& host, uint32_t timeout_ms) {
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (lwip_getaddrinfo(host.c_str(), "123", &hints, &found) != 0 || found == nullptr) {
        return Result<int64_t>::err(ErrorCode::NotFound, "cannot resolve " + host);
    }
    sockaddr_in server = {};
    std::memcpy(&server, found->ai_addr, sizeof(server));
    lwip_freeaddrinfo(found);

    SocketGuard sock{lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)};
    if (sock.fd < 0) return io_error("socket");
    // A connected UDP socket only receives datagrams from the server.
    if (lwip_connect(sock.fd, reinterpret_cast<sockaddr*>(&server), sizeof(server)) != 0) {
        return io_error("connect");
    }

    uint64_t nonce = 0;
    esp_fill_random(&nonce, sizeof(nonce));
    SntpPacket packet;
    sntp_request(packet, nonce);
    if (lwip_send(sock.fd, packet.data(), packet.size(), 0) != static_cast<int>(packet.size())) {
        return io_error("send");
    }

    const uint64_t deadline = os::now_ms() + timeout_ms;
    uint8_t buf[kSntpPacketSize + 64];
    while (true) {
        const uint64_t now = os::now_ms();
        if (now >= deadline) break;
        const uint64_t left = deadline - now;
        timeval tv = {};
        tv.tv_sec = static_cast<time_t>(left / 1000);
        tv.tv_usec = static_cast<suseconds_t>((left % 1000) * 1000);
        lwip_setsockopt(sock.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const int n = lwip_recv(sock.fd, buf, sizeof(buf), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return io_error("recv");
        }
        if (auto t = sntp_parse_reply(buf, static_cast<size_t>(n), nonce)) {
            return Result<int64_t>::ok(*t);
        }
    }
    return Result<int64_t>::err(ErrorCode::Timeout, "no valid answer from " + host);
}

void set_wall_clock(int64_t unix_s) {
    timeval tv = {};
    tv.tv_sec = static_cast<time_t>(unix_s);
    settimeofday(&tv, nullptr);
}

}  // namespace wisp::port::esp
