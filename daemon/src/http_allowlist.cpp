#include "http_allowlist.h"

#include "string_util.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string>
#include <vector>

namespace agentd {
namespace {

struct HostPort {
  std::string host;     // lowercased; IPv6 without brackets
  int port = -1;        // -1 means "any"
  bool has_port = false;
};

static bool parse_hostport(const std::string& s_in, HostPort* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  *out = HostPort{};

  const std::string s = trim_copy(s_in);
  if (s.empty()) {
    if (out_err) *out_err = "empty host entry";
    return false;
  }

  std::string host;
  std::string port_s;
  if (!s.empty() && s[0] == '[') {
    // [IPv6]:port
    const size_t rb = s.find(']');
    if (rb == std::string::npos) {
      if (out_err) *out_err = "invalid IPv6 host entry (missing ']')";
      return false;
    }
    host = s.substr(1, rb - 1);
    if (rb + 1 < s.size()) {
      if (s[rb + 1] != ':') {
        if (out_err) *out_err = "invalid IPv6 host entry (expected :port after ']')";
        return false;
      }
      port_s = s.substr(rb + 2);
    }
  } else {
    const size_t col = s.rfind(':');
    if (col != std::string::npos && s.find(':') == col) {
      // host:port (single colon)
      host = s.substr(0, col);
      port_s = s.substr(col + 1);
    } else {
      // host (or raw IPv6 without brackets, which we do not accept in allowlist for clarity)
      host = s;
    }
  }

  host = trim_copy(host);
  port_s = trim_copy(port_s);
  if (host.empty()) {
    if (out_err) *out_err = "missing host";
    return false;
  }

  // Lowercase host for matching.
  for (char& c : host) c = (char)std::tolower((unsigned char)c);

  if (!port_s.empty()) {
    // parse port
    try {
      const int p = std::stoi(port_s);
      if (p < 1 || p > 65535) {
        if (out_err) *out_err = "invalid port";
        return false;
      }
      out->has_port = true;
      out->port = p;
    } catch (...) {
      if (out_err) *out_err = "invalid port";
      return false;
    }
  } else {
    out->has_port = false;
    out->port = -1;
  }

  out->host = host;
  return true;
}

static bool parse_http_url_target_hostport(const std::string& url_in, HostPort* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  *out = HostPort{};

  const std::string url = trim_copy(url_in);
  const size_t scheme = url.find("://");
  if (scheme == std::string::npos) {
    if (out_err) *out_err = "invalid url (missing scheme)";
    return false;
  }
  const std::string scheme_s = lower_copy(url.substr(0, scheme));
  if (scheme_s != "http" && scheme_s != "https") {
    if (out_err) *out_err = "invalid url scheme (expected http or https)";
    return false;
  }

  size_t i = scheme + 3;
  // Skip optional userinfo "user:pass@" (best-effort; we do not allow it in allowlist entries, but URLs might contain it).
  // Find first /?# terminator, then consider the host portion within that.
  size_t end = url.find_first_of("/?#", i);
  if (end == std::string::npos) end = url.size();
  std::string authority = url.substr(i, end - i);
  const size_t at = authority.rfind('@');
  if (at != std::string::npos) {
    authority = authority.substr(at + 1);
  }
  authority = trim_copy(authority);
  if (authority.empty()) {
    if (out_err) *out_err = "invalid url (missing host)";
    return false;
  }

  std::string host;
  std::string port_s;
  if (!authority.empty() && authority[0] == '[') {
    const size_t rb = authority.find(']');
    if (rb == std::string::npos) {
      if (out_err) *out_err = "invalid url (bad IPv6 host)";
      return false;
    }
    host = authority.substr(1, rb - 1);
    if (rb + 1 < authority.size() && authority[rb + 1] == ':') {
      port_s = authority.substr(rb + 2);
    }
  } else {
    const size_t col = authority.rfind(':');
    if (col != std::string::npos && authority.find(':') == col) {
      host = authority.substr(0, col);
      port_s = authority.substr(col + 1);
    } else {
      host = authority;
    }
  }
  host = trim_copy(host);
  port_s = trim_copy(port_s);
  if (host.empty()) {
    if (out_err) *out_err = "invalid url (empty host)";
    return false;
  }
  for (char& c : host) c = (char)std::tolower((unsigned char)c);

  int port = (scheme_s == "https") ? 443 : 80;
  bool has_port = false;
  if (!port_s.empty()) {
    try {
      const int p = std::stoi(port_s);
      if (p < 1 || p > 65535) {
        if (out_err) *out_err = "invalid url port";
        return false;
      }
      port = p;
      has_port = true;
    } catch (...) {
      if (out_err) *out_err = "invalid url port";
      return false;
    }
  }

  out->host = host;
  out->port = port;
  out->has_port = has_port;
  return true;
}

static std::string hostport_to_string(const HostPort& hp) {
  if (hp.host.find(':') != std::string::npos) {
    // IPv6
    if (hp.port > 0) return "[" + hp.host + "]:" + std::to_string(hp.port);
    return "[" + hp.host + "]";
  }
  if (hp.port > 0) return hp.host + ":" + std::to_string(hp.port);
  return hp.host;
}

struct IpAddr {
  int family = 0; // AF_INET or AF_INET6
  uint8_t bytes[16]{};
};

static bool ip_parse_literal(const std::string& host, IpAddr* out) {
  if (!out) return false;
  *out = IpAddr{};
  IpAddr ip;
  ip.family = AF_INET;
  if (::inet_pton(AF_INET, host.c_str(), ip.bytes) == 1) {
    *out = ip;
    return true;
  }
  ip = IpAddr{};
  ip.family = AF_INET6;
  if (::inet_pton(AF_INET6, host.c_str(), ip.bytes) == 1) {
    *out = ip;
    return true;
  }
  return false;
}

static bool ip_is_private_or_loopback(const IpAddr& ip) {
  if (ip.family == AF_INET) {
    const uint8_t a = ip.bytes[0];
    const uint8_t b = ip.bytes[1];
    // 10.0.0.0/8
    if (a == 10) return true;
    // 127.0.0.0/8
    if (a == 127) return true;
    // 169.254.0.0/16 (link-local)
    if (a == 169 && b == 254) return true;
    // 172.16.0.0/12
    if (a == 172 && (b >= 16 && b <= 31)) return true;
    // 192.168.0.0/16
    if (a == 192 && b == 168) return true;
    // 0.0.0.0/8
    if (a == 0) return true;
    // 100.64.0.0/10 (CGNAT)
    if (a == 100 && (b >= 64 && b <= 127)) return true;
    // 192.0.2.0/24, 198.51.100.0/24, 203.0.113.0/24 (TEST-NET)
    if (a == 192 && b == 0 && ip.bytes[2] == 2) return true;
    if (a == 198 && b == 51 && ip.bytes[2] == 100) return true;
    if (a == 203 && b == 0 && ip.bytes[2] == 113) return true;
    // 224.0.0.0/4 multicast and 240.0.0.0/4 reserved
    if (a >= 224) return true;
    return false;
  }
  if (ip.family == AF_INET6) {
    // ::1 loopback
    bool all0 = true;
    for (int i = 0; i < 15; i++) if (ip.bytes[i] != 0) { all0 = false; break; }
    if (all0 && ip.bytes[15] == 1) return true;
    // ::/128 unspecified
    bool allz = true;
    for (int i = 0; i < 16; i++) if (ip.bytes[i] != 0) { allz = false; break; }
    if (allz) return true;
    // fc00::/7 (ULA)
    if ((ip.bytes[0] & 0xfe) == 0xfc) return true;
    // fe80::/10 (link-local)
    if (ip.bytes[0] == 0xfe && (ip.bytes[1] & 0xc0) == 0x80) return true;
    // ff00::/8 multicast
    if (ip.bytes[0] == 0xff) return true;
    return false;
  }
  return true;
}

struct Cidr {
  int family = 0;
  uint8_t bytes[16]{};
  int prefix = 0;
};

static bool cidr_parse(const std::string& s_in, Cidr* out, std::string* out_err) {
  if (out_err) out_err->clear();
  if (!out) return false;
  *out = Cidr{};
  const std::string s = trim_copy(s_in);
  const size_t slash = s.find('/');
  if (slash == std::string::npos) {
    if (out_err) *out_err = "missing /prefix";
    return false;
  }
  const std::string host = trim_copy(s.substr(0, slash));
  const std::string pref_s = trim_copy(s.substr(slash + 1));
  if (host.empty() || pref_s.empty()) {
    if (out_err) *out_err = "invalid cidr";
    return false;
  }
  int pref = 0;
  try {
    pref = std::stoi(pref_s);
  } catch (...) {
    if (out_err) *out_err = "invalid prefix";
    return false;
  }
  IpAddr ip;
  if (!ip_parse_literal(host, &ip)) {
    if (out_err) *out_err = "invalid cidr address";
    return false;
  }
  const int max_pref = (ip.family == AF_INET) ? 32 : 128;
  if (pref < 0 || pref > max_pref) {
    if (out_err) *out_err = "invalid prefix";
    return false;
  }
  out->family = ip.family;
  std::memcpy(out->bytes, ip.bytes, sizeof(out->bytes));
  out->prefix = pref;
  return true;
}

static bool ip_in_cidr(const IpAddr& ip, const Cidr& c) {
  if (ip.family != c.family) return false;
  const int bits = c.prefix;
  const int full = bits / 8;
  const int rem = bits % 8;
  if (full > 0) {
    if (std::memcmp(ip.bytes, c.bytes, (size_t)full) != 0) return false;
  }
  if (rem == 0) return true;
  const uint8_t mask = (uint8_t)(0xffu << (8 - rem));
  return (ip.bytes[full] & mask) == (c.bytes[full] & mask);
}

static void resolve_host_best_effort(const std::string& host, std::vector<IpAddr>* out) {
  if (!out) return;
  out->clear();
  if (host.empty()) return;

  // If it's a literal IP, skip DNS.
  IpAddr ip;
  if (ip_parse_literal(host, &ip)) {
    out->push_back(ip);
    return;
  }

  struct addrinfo hints;
  std::memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_ADDRCONFIG;

  struct addrinfo* res = nullptr;
  const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
  if (rc != 0 || !res) return;
  int kept = 0;
  for (struct addrinfo* p = res; p; p = p->ai_next) {
    if (kept >= 16) break;
    if (!p->ai_addr) continue;
    if (p->ai_family == AF_INET) {
      const struct sockaddr_in* sin = (const struct sockaddr_in*)p->ai_addr;
      IpAddr a;
      a.family = AF_INET;
      std::memcpy(a.bytes, &sin->sin_addr, 4);
      out->push_back(a);
      kept++;
    } else if (p->ai_family == AF_INET6) {
      const struct sockaddr_in6* sin6 = (const struct sockaddr_in6*)p->ai_addr;
      IpAddr a;
      a.family = AF_INET6;
      std::memcpy(a.bytes, &sin6->sin6_addr, 16);
      out->push_back(a);
      kept++;
    }
  }
  ::freeaddrinfo(res);
}

static bool allow_hosts_match(const std::vector<std::string>& allow_hosts, const HostPort& target) {
  for (const auto& entry_raw : allow_hosts) {
    HostPort allow;
    std::string aerr;
    if (!parse_hostport(entry_raw, &allow, &aerr)) continue;
    if (allow.host != target.host) continue;
    if (!allow.has_port) return true;
    if (allow.port == target.port) return true;
  }
  return false;
}

static bool allow_cidrs_match(const std::vector<std::string>& allow_cidrs, const std::vector<IpAddr>& addrs) {
  if (allow_cidrs.empty()) return false;
  std::vector<Cidr> parsed;
  parsed.reserve(allow_cidrs.size());
  for (const auto& raw : allow_cidrs) {
    Cidr c;
    std::string err;
    if (cidr_parse(raw, &c, &err)) parsed.push_back(c);
  }
  if (parsed.empty()) return false;
  for (const auto& ip : addrs) {
    for (const auto& c : parsed) {
      if (ip_in_cidr(ip, c)) return true;
    }
  }
  return false;
}

static bool any_private(const std::vector<IpAddr>& addrs) {
  for (const auto& ip : addrs) {
    if (ip_is_private_or_loopback(ip)) return true;
  }
  return false;
}

static bool host_is_literal_ip(const std::string& host) {
  IpAddr ip;
  return ip_parse_literal(host, &ip);
}

}  // namespace

bool workflow_http_url_is_allowed(
  const DaemonConfig& cfg,
  const std::string& url,
  std::string* out_reason
) {
  if (out_reason) out_reason->clear();
  const bool has_hosts = !cfg.workflow_http_allow_hosts.empty();
  const bool has_cidrs = !cfg.workflow_http_allow_cidrs.empty();
  if (!has_hosts && !has_cidrs && !cfg.workflow_http_deny_private_addrs) return true;

  HostPort target;
  std::string terr;
  if (!parse_http_url_target_hostport(url, &target, &terr)) {
    if (out_reason) *out_reason = "failed to parse url for allowlist check: " + terr;
    return false;
  }

  const bool host_match = has_hosts && allow_hosts_match(cfg.workflow_http_allow_hosts, target);

  std::vector<IpAddr> addrs;
  resolve_host_best_effort(target.host, &addrs);
  const bool cidr_match = has_cidrs && allow_cidrs_match(cfg.workflow_http_allow_cidrs, addrs);

  const bool allow_match = host_match || cidr_match;

  if (cfg.workflow_http_deny_private_addrs) {
    // Extra guard: deny obvious loopback/private unless explicitly allowed.
    // - If the target is a literal IP and it is explicitly allow-host matched, allow.
    // - If it matches an allowed CIDR, allow.
    // - Otherwise, deny if any resolved address is private/loopback/link-local.
    const bool target_is_ip = host_is_literal_ip(target.host);
    if (!target_is_ip && addrs.empty()) {
      if (out_reason) *out_reason = "dns resolution failed for host (deny-private enabled)";
      return false;
    }
    if (any_private(addrs)) {
      if (cidr_match) {
        return true;
      }
      if (host_match && target_is_ip) {
        return true;
      }
      if (out_reason) *out_reason = "target resolves to private/loopback address";
      return false;
    }
    // If allowlists are empty, and it isn't private, allow.
    if (!has_hosts && !has_cidrs) return true;
  }

  if (!has_hosts && !has_cidrs) return true;
  if (allow_match) return true;

  if (out_reason) {
    *out_reason = "url target not in allowlist: " + hostport_to_string(target);
  }
  return false;
}

}  // namespace agentd
