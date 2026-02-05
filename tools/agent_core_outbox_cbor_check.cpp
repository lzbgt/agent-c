#include "agent/um_eais_outbox_read.h"
#include "agent/um_eais_platform_caps_req_read.h"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cstring>

static bool text_eq(agent_cbor_text_view_t v, const std::string& s) {
  if (v.len != s.size()) return false;
  if (v.len == 0) return true;
  return std::memcmp(v.ptr, s.data(), v.len) == 0;
}

static std::vector<uint8_t> read_all(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  f.seekg(0, std::ios::end);
  const std::streamoff n = f.tellg();
  if (n <= 0) return {};
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> buf((size_t)n);
  f.read(reinterpret_cast<char*>(buf.data()), n);
  if (!f) return {};
  return buf;
}

static void usage(const char* argv0) {
  std::cerr
    << "usage: " << (argv0 ? argv0 : "agent_core_outbox_cbor_check")
    << " --file <path> --node-id <id> --expect-type <UM_BMP_TYPE>\n";
}

int main(int argc, char** argv) {
  std::string path;
  std::string node_id;
  std::string expect_type;

  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i] ? argv[i] : "";
    if (a == "--file" && i + 1 < argc) {
      path = argv[++i];
      continue;
    }
    if (a == "--node-id" && i + 1 < argc) {
      node_id = argv[++i];
      continue;
    }
    if (a == "--expect-type" && i + 1 < argc) {
      expect_type = argv[++i];
      continue;
    }
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return 0;
    }
    std::cerr << "unknown arg: " << a << "\n";
    usage(argv[0]);
    return 2;
  }

  if (path.empty() || node_id.empty() || expect_type.empty()) {
    usage(argv[0]);
    return 2;
  }

  const auto bytes = read_all(path);
  if (bytes.empty()) {
    std::cerr << "failed to read: " << path << "\n";
    return 1;
  }

  agent_um_eais_outbox_row_view_t rows[64];
  agent_um_eais_outbox_view_t out{};
  const agent_status_t st = agent_um_eais_outbox_read_cbor_v0_1(
    bytes.data(), bytes.size(),
    &out,
    rows,
    sizeof(rows) / sizeof(rows[0])
  );
  if (st != AGENT_OK) {
    std::cerr << "outbox decode failed status=" << (int)st << "\n";
    return 1;
  }

  if (!text_eq(out.node_id, node_id)) {
    std::cerr << "unexpected node_id\n";
    return 1;
  }

  bool saw = false;
  bool saw_valid_caps_req = false;
  for (size_t i = 0; i < out.messages_parsed; i++) {
    if (!rows[i].has_msg) continue;
    if (text_eq(rows[i].msg.type, expect_type)) {
      saw = true;
      if (expect_type == "PLATFORM_CAPS_REQ") {
        if (!rows[i].msg.has_body || !rows[i].msg.body_item.ptr || rows[i].msg.body_item.len == 0) {
          std::cerr << "PLATFORM_CAPS_REQ missing body\n";
          return 1;
        }
        agent_um_eais_platform_caps_req_view_t req{};
        const agent_status_t st2 = agent_um_eais_platform_caps_req_body_read_cbor_v0_1(
          rows[i].msg.body_item.ptr,
          rows[i].msg.body_item.len,
          &req
        );
        if (st2 != AGENT_OK) {
          std::cerr << "PLATFORM_CAPS_REQ body decode failed status=" << (int)st2 << "\n";
          return 1;
        }
        if (!text_eq(req.node_id, node_id)) {
          std::cerr << "PLATFORM_CAPS_REQ body.node_id mismatch\n";
          return 1;
        }
        if (!text_eq(req.want, "full")) {
          std::cerr << "PLATFORM_CAPS_REQ body.want expected \"full\"\n";
          return 1;
        }
        saw_valid_caps_req = true;
      }
      break;
    }
  }

  if (!saw) {
    std::cerr << "expected type not found: " << expect_type << "\n";
    std::cerr << "parsed=" << out.messages_parsed << " total=" << out.messages_total << "\n";
    for (size_t i = 0; i < out.messages_parsed; i++) {
      if (!rows[i].has_msg) continue;
      std::string t(rows[i].msg.type.ptr ? rows[i].msg.type.ptr : "", rows[i].msg.type.len);
      std::cerr << "  type[" << i << "]=" << t << "\n";
    }
    return 1;
  }

  if (expect_type == "PLATFORM_CAPS_REQ" && !saw_valid_caps_req) {
    std::cerr << "PLATFORM_CAPS_REQ found but not validated\n";
    return 1;
  }

  return 0;
}
