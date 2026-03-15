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

static bool base64_decode(const std::string& in, std::vector<uint8_t>* out) {
  if (!out) return false;
  out->clear();
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return 26 + (c - 'a');
    if (c >= '0' && c <= '9') return 52 + (c - '0');
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
  };
  if (in.empty() || (in.size() % 4) != 0) return false;
  out->reserve((in.size() / 4) * 3);
  for (size_t i = 0; i < in.size(); i += 4) {
    const int a = val(in[i]);
    const int b = val(in[i + 1]);
    const int c = val(in[i + 2]);
    const int d = val(in[i + 3]);
    if (a < 0 || b < 0 || c == -1 || d == -1) return false;
    const uint32_t n =
      ((uint32_t)(a & 63) << 18) |
      ((uint32_t)(b & 63) << 12) |
      ((uint32_t)((c < 0 ? 0 : c) & 63) << 6) |
      ((uint32_t)((d < 0 ? 0 : d) & 63));
    out->push_back((uint8_t)((n >> 16) & 0xffu));
    if (c != -2) out->push_back((uint8_t)((n >> 8) & 0xffu));
    if (d != -2) out->push_back((uint8_t)(n & 0xffu));
    if ((c == -2 && d != -2) || (i + 4 != in.size() && (c == -2 || d == -2))) return false;
  }
  return true;
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
  std::string pk_b64;
  std::string sig_b64;
  bool print_pk_b64 = false;
  bool verify = false;

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    if (a == "--sk-hex") {
      if (i + 1 >= argc) die("missing value for --sk-hex");
      sk_hex = argv[++i] ? argv[i] : "";
    } else if (a == "--pk-b64") {
      if (i + 1 >= argc) die("missing value for --pk-b64");
      pk_b64 = argv[++i] ? argv[i] : "";
    } else if (a == "--sig-b64") {
      if (i + 1 >= argc) die("missing value for --sig-b64");
      sig_b64 = argv[++i] ? argv[i] : "";
    } else if (a == "--print-pk-b64") {
      print_pk_b64 = true;
    } else if (a == "--verify") {
      verify = true;
    } else if (a == "--help" || a == "-h") {
      std::fprintf(stderr,
        "Usage:\n"
        "  agent_ed25519_tool --sk-hex <64hex> --print-pk-b64\n"
        "  agent_ed25519_tool --sk-hex <64hex>  # reads message bytes from stdin, prints signature base64\n"
        "  agent_ed25519_tool --verify --pk-b64 <base64(pubkey32)> --sig-b64 <base64(sig64)>\n");
      return 0;
    } else {
      die("unknown arg");
    }
  }

  if (verify) {
    if (!sk_hex.empty() || print_pk_b64) die("--verify cannot be combined with signing options");
    std::vector<uint8_t> pk;
    std::vector<uint8_t> sig;
    if (!base64_decode(pk_b64, &pk) || pk.size() != 32) die("invalid --pk-b64 (expected base64 of 32 bytes)");
    if (!base64_decode(sig_b64, &sig) || sig.size() != 64) die("invalid --sig-b64 (expected base64 of 64 bytes)");
    const std::string msg = read_all_stdin();
    const int ok = agent_ed25519_verify(msg.data(), msg.size(), pk.data(), sig.data());
    if (!ok) return 1;
    std::printf("OK\n");
    return 0;
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
