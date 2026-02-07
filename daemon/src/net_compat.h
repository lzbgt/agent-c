#pragma once

#include <cstddef>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
using socket_io_t = int;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
constexpr socket_io_t kSocketError = SOCKET_ERROR;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
using socket_t = int;
using socket_io_t = ssize_t;
constexpr socket_t kInvalidSocket = -1;
constexpr socket_io_t kSocketError = -1;
#endif

namespace agentd {

// Initializes network stack where required (Windows). Safe to call multiple times.
bool net_init(std::string* out_error = nullptr);

int socket_last_error();
std::string socket_strerror(int err);
bool socket_should_retry(int err);

socket_io_t socket_read(socket_t s, void* buf, size_t len);
socket_io_t socket_write(socket_t s, const void* buf, size_t len);
void socket_close(socket_t s);
bool socket_shutdown(socket_t s);
bool socket_valid(socket_t s);

}  // namespace agentd
