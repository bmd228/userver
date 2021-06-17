#include <clients/config/client.hpp>

#include <clients/http/client.hpp>
#include <formats/json/serialize.hpp>
#include <formats/json/value_builder.hpp>
#include <logging/log.hpp>

namespace clients::taxi_config {
namespace {
const std::string kConfigsValues = "/configs/values";
}  // namespace

Client::Client(clients::http::Client& http_client, const ClientConfig& config)
    : config_(config), http_client_(http_client) {}

Client::~Client() = default;

std::string Client::FetchConfigsValues(const std::string& body) {
  const auto timeout_ms = config_.timeout.count();
  const auto retries = config_.retries;
  const auto url = config_.config_url + kConfigsValues;

  // Storing and overriding proxy below to avoid issues with concurrent update
  // of proxy runtime config.
  const auto proxy = http_client_.GetProxy();

  std::exception_ptr exception;
  try {
    auto reply = http_client_.CreateRequest()
                     ->post(url, body)
                     ->timeout(timeout_ms)
                     ->retry(retries)
                     ->proxy(proxy)
                     ->perform();
    reply->raise_for_status();
    return std::move(*reply).body();
  } catch (const clients::http::BaseException& /*e*/) {
    if (!config_.fallback_to_no_proxy || proxy.empty()) {
      throw;
    }
    exception = std::current_exception();
  }

  try {
    auto no_proxy_reply = http_client_.CreateRequest()
                              ->proxy({})
                              ->post(url, body)
                              ->timeout(timeout_ms)
                              ->retry(retries)
                              ->perform();

    if (no_proxy_reply->IsOk()) {
      LOG_WARNING() << "Using non proxy response in config client";
      return std::move(*no_proxy_reply).body();
    }
  } catch (const clients::http::BaseException& e) {
    LOG_WARNING() << "Non proxy request in config client failed: " << e;
  }

  std::rethrow_exception(exception);
}

Client::Reply Client::FetchDocsMap(
    const std::optional<Timestamp>& last_update,
    const std::vector<std::string>& fields_to_load) {
  auto source = config_.use_uconfigs ? Source::kUconfigs : Source::kConfigs;
  auto json_value = FetchConfigs(last_update, fields_to_load, source);
  auto configs_json = json_value["configs"];

  Reply reply;
  reply.docs_map.Parse(formats::json::ToString(configs_json), true);

  reply.timestamp = json_value["updated_at"].As<std::string>();
  return reply;
}

Client::Reply Client::DownloadFullDocsMap() {
  return FetchDocsMap(std::nullopt, {});
}

Client::JsonReply Client::FetchJson(
    const std::optional<Timestamp>& last_update,
    const std::unordered_set<std::string>& fields_to_load) {
  auto json_value = FetchConfigs(last_update, fields_to_load, Source::kConfigs);
  auto configs_json = json_value["configs"];

  JsonReply reply;
  reply.configs = configs_json;

  reply.timestamp = json_value["updated_at"].As<std::string>();
  return reply;
}

formats::json::Value Client::FetchConfigs(
    const std::optional<Timestamp>& last_update,
    formats::json::ValueBuilder&& fields_to_load, Source source) {
  formats::json::ValueBuilder body_builder(formats::json::Type::kObject);

  if (!fields_to_load.IsEmpty()) {
    body_builder["ids"] = std::move(fields_to_load);
  }

  if (last_update) {
    body_builder["updated_since"] = *last_update;
  }

  if (source == Source::kUconfigs) {
    body_builder["stage_name"] = config_.stage_name;
  }

  if (config_.get_configs_overrides_for_service) {
    body_builder["service"] = config_.service_name;
  }

  auto request_body = formats::json::ToString(body_builder.ExtractValue());
  LOG_DEBUG() << "request body: " << request_body;

  auto json = FetchConfigsValues(request_body);

  return formats::json::FromString(json);
}

}  // namespace clients::taxi_config
