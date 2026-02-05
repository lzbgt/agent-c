#include "http_allowlist.h"

#include "string_util.h"

#include <algorithm>
#include <cctype>
#include <string>

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

}  // namespace

bool workflow_http_url_is_allowed(
  const DaemonConfig& cfg,
  const std::string& url,
  std::string* out_reason
) {
  if (out_reason) out_reason->clear();
  if (cfg.workflow_http_allow_hosts.empty()) return true;

  HostPort target;
  std::string terr;
  if (!parse_http_url_target_hostport(url, &target, &terr)) {
    if (out_reason) *out_reason = "failed to parse url for allowlist check: " + terr;
    return false;
  }

  for (const auto& entry_raw : cfg.workflow_http_allow_hosts) {
    HostPort allow;
    std::string aerr;
    if (!parse_hostport(entry_raw, &allow, &aerr)) continue;
    if (allow.host != target.host) continue;
    if (!allow.has_port) return true;
    if (allow.port == target.port) return true;
  }

  if (out_reason) {
    *out_reason = "url target not in allowlist: " + hostport_to_string(target);
  }
  return false;
}

}  // namespace agentd

