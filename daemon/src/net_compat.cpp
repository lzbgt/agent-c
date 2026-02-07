#include "net_compat.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace agentd {

bool net_init(std::string* out_error) {
#if defined(_WIN32)
  static std::once_flag once;
  static bool ok = true;
  static std::string err;
  std::call_once(once, []() {
    WSADATA wsa{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
      ok = false;
      err = "WSAStartup failed: " + std::to_string(rc);
    }
  });
  if (out_error) *out_error = ok ? std::string() : err;
  return ok;
#else
  if (out_error) out_error->clear();
  return true;
#endif
}

int socket_last_error() {
#if defined(_WIN32)
  return (int)WSAGetLastError();
#else
  return errno;
#endif
}

std::string socket_strerror(int err) {
#if defined(_WIN32)
  char* msg = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
  const DWORD len = FormatMessageA(flags, nullptr, (DWORD)err, lang, (LPSTR)&msg, 0, nullptr);
  std::string out;
  if (len && msg) {
    out.assign(msg, msg + len);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
  } else {
    out = "winsock error " + std::to_string(err);
  }
  if (msg) LocalFree(msg);
  return out;
#else
  return std::strerror(err);
#endif
}

bool socket_should_retry(int err) {
#if defined(_WIN32)
  return err == WSAEINTR;
#else
  return err == EINTR;
#endif
}

socket_io_t socket_read(socket_t s, void* buf, size_t len) {
#if defined(_WIN32)
  if (!buf || len == 0) return 0;
  const int want = (len > (size_t)INT_MAX) ? INT_MAX : (int)len;
  return ::recv(s, (char*)buf, want, 0);
#else
  return ::read(s, buf, len);
#endif
}

socket_io_t socket_write(socket_t s, const void* buf, size_t len) {
#if defined(_WIN32)
  if (!buf || len == 0) return 0;
  const int want = (len > (size_t)INT_MAX) ? INT_MAX : (int)len;
  return ::send(s, (const char*)buf, want, 0);
#else
  return ::write(s, buf, len);
#endif
}

void socket_close(socket_t s) {
  if (!socket_valid(s)) return;
#if defined(_WIN32)
  ::closesocket(s);
#else
  ::close(s);
#endif
}

bool socket_shutdown(socket_t s) {
  if (!socket_valid(s)) return false;
#if defined(_WIN32)
  return ::shutdown(s, SD_BOTH) == 0;
#else
  return ::shutdown(s, SHUT_RDWR) == 0;
#endif
}

bool socket_valid(socket_t s) {
  return s != kInvalidSocket;
}

}  // namespace agentd
