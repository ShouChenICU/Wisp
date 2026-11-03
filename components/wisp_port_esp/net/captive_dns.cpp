#include "captive_dns.hpp"

#include <cerrno>
#include <cstring>

#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/dns_reply.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "captive_dns";
constexpr uint16_t kDnsPort = 53;

}  // namespace

CaptiveDns::~CaptiveDns() { stop(); }

Result<void> CaptiveDns::start(uint32_t ip) {
    if (task_) return Result<void>::ok();

    int sock = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return Result<void>::err(ErrorCode::IoError, "socket: errno " + std::to_string(errno));
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = lwip_htons(kDnsPort);
    addr.sin_addr.s_addr = lwip_htonl(ip);
    if (lwip_bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        lwip_close(sock);
        return Result<void>::err(ErrorCode::IoError, "bind :53: errno " + std::to_string(err));
    }
    // The receive timeout is what lets stop() end the task: a blocked
    // receive cannot be interrupted, and closing a socket that another
    // task is blocked on is not safe in lwIP.
    timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = kPollMs * 1000;
    lwip_setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    stop_.store(false);
    task_ = os::Task::spawn({.name = "wisp.dns", .stack_bytes = 3072, .priority = 4},
                            [this, sock, ip] { run(sock, ip); });
    if (!task_) {
        lwip_close(sock);
        return Result<void>::err(ErrorCode::NoMemory, "could not start the DNS task");
    }
    WLOG_I(kTag, "answering DNS on port %u", static_cast<unsigned>(kDnsPort));
    return Result<void>::ok();
}

void CaptiveDns::stop() {
    if (!task_) return;
    stop_.store(true);
    task_->join();
    task_.reset();
    WLOG_I(kTag, "stopped");
}

void CaptiveDns::run(int sock, uint32_t ip) {
    // One query/answer at a time; both fit the classic 512-byte limit.
    uint8_t query[kDnsMaxMessage];
    uint8_t answer[kDnsMaxMessage];
    while (!stop_.load()) {
        sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        const int n = lwip_recvfrom(sock, query, sizeof(query), 0,
                                    reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) continue;  // timeout (the stop check) or a transient error
        const size_t len =
            captive_dns_reply(query, static_cast<size_t>(n), ip, answer, sizeof(answer));
        if (len == 0) continue;
        lwip_sendto(sock, answer, len, 0, reinterpret_cast<sockaddr*>(&from), from_len);
    }
    lwip_close(sock);
}

}  // namespace wisp::port::esp
