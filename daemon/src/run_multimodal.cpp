#include "run_multimodal.h"

#include <cstring>
#include <sstream>

namespace agentd {

const char* kMultimodalPrefix = "__AGENT_MM_V1__";

bool try_parse_multimodal_prefix(const std::string& content, Json::Value* out_mm, std::string* out_text) {
  if (out_mm) *out_mm = Json::Value(Json::nullValue);
  if (out_text) *out_text = content;
  if (!out_mm || !out_text) return false;
  if (content.rfind(kMultimodalPrefix, 0) != 0) return false;
  const size_t nl = content.find('\n');
  if (nl == std::string::npos) return false;
  const std::string json_part = content.substr(std::strlen(kMultimodalPrefix), nl - std::strlen(kMultimodalPrefix));
  if (json_part.empty()) return false;
  Json::CharReaderBuilder rb;
  std::string errs;
  std::istringstream iss(json_part);
  Json::Value v;
  if (!Json::parseFromStream(rb, iss, &v, &errs) || !v.isObject()) return false;
  *out_mm = v;
  *out_text = content.substr(nl + 1);
  return true;
}

Json::Value multimodal_content_from_parts(const std::string& text, const Json::Value& mm, bool allow_image_parts) {
  const bool have_images = mm.isMember("images") && mm["images"].isArray() && !mm["images"].empty();
  const bool have_files = mm.isMember("files") && mm["files"].isArray() && !mm["files"].empty();
  if (!have_images && !have_files) return Json::Value(text);

  Json::Value arr(Json::arrayValue);
  if (!text.empty()) {
    Json::Value t(Json::objectValue);
    t["type"] = "text";
    t["text"] = text;
    arr.append(t);
  }
  if (have_files) {
    for (const auto& f : mm["files"]) {
      if (!f.isObject()) continue;
      const std::string name = f.isMember("name") && f["name"].isString() ? f["name"].asString() : "";
      const std::string mime = f.isMember("mime") && f["mime"].isString() ? f["mime"].asString() : "";
      const std::string ft = f.isMember("text") && f["text"].isString() ? f["text"].asString() : "";
      const bool trunc = f.isMember("truncated") && f["truncated"].isBool() ? f["truncated"].asBool() : false;
      if (ft.empty()) continue;
      std::string block;
      block += "[Attachment";
      if (!name.empty()) block += ": " + name;
      if (!mime.empty()) block += " (" + mime + ")";
      block += "]\n";
      block += ft;
      if (trunc) block += "\n...(truncated)";
      Json::Value t(Json::objectValue);
      t["type"] = "text";
      t["text"] = block;
      arr.append(t);
    }
  }
  if (have_images) {
    for (const auto& im : mm["images"]) {
      if (!im.isObject()) continue;
      const std::string name = im.isMember("name") && im["name"].isString() ? im["name"].asString() : "";
      const std::string mime = im.isMember("mime") && im["mime"].isString() ? im["mime"].asString() : "image/png";
      const std::string b64 = im.isMember("b64") && im["b64"].isString() ? im["b64"].asString() : "";
      if (b64.empty()) continue;
      if (allow_image_parts) {
        const std::string url = std::string("data:") + mime + ";base64," + b64;
        Json::Value part(Json::objectValue);
        part["type"] = "image_url";
        Json::Value iu(Json::objectValue);
        iu["url"] = url;
        part["image_url"] = iu;
        arr.append(part);
      } else {
        std::string hint;
        hint += "[Image attachment";
        if (!name.empty()) hint += ": " + name;
        if (!mime.empty()) hint += " (" + mime + ")";
        hint += "]\n";
        hint += "(Image omitted: provider does not accept image_url content parts.)";
        Json::Value part(Json::objectValue);
        part["type"] = "text";
        part["text"] = hint;
        arr.append(part);
      }
    }
  }
  return arr;
}

}  // namespace agentd

