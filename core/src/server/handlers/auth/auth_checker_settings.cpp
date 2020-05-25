#include <server/handlers/auth/auth_checker_settings.hpp>

#include <stdexcept>

#include <fmt/format.h>

#include <formats/json/serialize.hpp>
#include <logging/log.hpp>

namespace {

const std::string kApikeys = "apikeys";

}  // namespace

namespace server {
namespace handlers {
namespace auth {

AuthCheckerSettings::AuthCheckerSettings(const formats::json::Value& doc) {
  if (doc.HasMember(kApikeys)) {
    ParseApikeys(doc[kApikeys]);
  }
}

void AuthCheckerSettings::ParseApikeys(
    const formats::json::Value& apikeys_map) {
  if (!apikeys_map.IsObject())
    throw std::runtime_error("cannot parse " + kApikeys + ", object expected");

  apikeys_map_ = std::make_optional<ApiKeysMap>({});

  for (const auto& [apikey_type, elem] : Items(apikeys_map)) {
    if (!elem.IsArray())
      throw std::runtime_error(
          fmt::format("cannot parse {}.{}, expected", kApikeys, apikey_type));
    for (auto key = elem.begin(); key != elem.end(); ++key) {
      if (key->IsString()) {
        (*apikeys_map_)[apikey_type].insert(key->As<std::string>());
      } else {
        throw std::runtime_error(
            fmt::format("cannot parse {}.{}[{}], string expected", kApikeys,
                        apikey_type, std::to_string(key.GetIndex())));
      }
    }
  }
}

}  // namespace auth
}  // namespace handlers
}  // namespace server
