#include "agent/ed25519.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static void die(const char* msg) {
  if (!msg) msg = "error";
  std::fprintf(stderr, "%s\n", msg);
  std::exit(2);
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

static bool hex_decode_32(const std::string& hex, uint8_t out32[32]) {
  if (!out32) return false;
  if (hex.size() != 64) return false;
  for (size_t i = 0; i < 32; i++) {
    const int hi = hex_val(hex[2 * i]);
    const int lo = hex_val(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out32[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static std::string base64_encode(const uint8_t* data, size_t len) {
  static const char* k =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < len) ? data[i + 1] : 0;
    const uint32_t b2 = (i + 2 < len) ? data[i + 2] : 0;
    const uint32_t n = (b0 << 16) | (b1 << 8) | b2;

    out.push_back(k[(n >> 18) & 63]);
    out.push_back(k[(n >> 12) & 63]);
    out.push_back((i + 1 < len) ? k[(n >> 6) & 63] : '=');
    out.push_back((i + 2 < len) ? k[n & 63] : '=');
  }
  return out;
}

static std::string read_all_stdin(void) {
  std::string out;
  std::vector<char> buf(4096);
  while (true) {
    const size_t n = std::fread(buf.data(), 1, buf.size(), stdin);
    if (n > 0) out.append(buf.data(), n);
    if (n < buf.size()) break;
  }
  return out;
}

int main(int argc, char** argv) {
  std::string sk_hex;
  bool print_pk_b64 = false;

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    if (a == "--sk-hex") {
      if (i + 1 >= argc) die("missing value for --sk-hex");
      sk_hex = argv[++i] ? argv[i] : "";
    } else if (a == "--print-pk-b64") {
      print_pk_b64 = true;
    } else if (a == "--help" || a == "-h") {
      std::fprintf(stderr,
        "Usage:\n"
        "  agent_ed25519_tool --sk-hex <64hex> --print-pk-b64\n"
        "  agent_ed25519_tool --sk-hex <64hex>  # reads message bytes from stdin, prints signature base64\n");
      return 0;
    } else {
      die("unknown arg");
    }
  }

  uint8_t sk[32];
  if (!hex_decode_32(sk_hex, sk)) die("invalid --sk-hex (expected 64 hex chars)");

  uint8_t pk[32];
  std::memset(pk, 0, sizeof(pk));
  agent_ed25519_publickey(sk, pk);

  if (print_pk_b64) {
    const std::string b64 = base64_encode(pk, sizeof(pk));
    std::printf("%s\n", b64.c_str());
    return 0;
  }

  const std::string msg = read_all_stdin();
  uint8_t sig[64];
  std::memset(sig, 0, sizeof(sig));
  agent_ed25519_sign(msg.data(), msg.size(), sk, pk, sig);
  const std::string b64 = base64_encode(sig, sizeof(sig));
  std::printf("%s\n", b64.c_str());
  return 0;
}

