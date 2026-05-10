#include <dockyard/log.hpp>

#include <memory>
#include <mutex>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace dy {
namespace {
std::shared_ptr<spdlog::logger> logger;
std::once_flag init_flag;
} // namespace

static spdlog::level::level_enum to_spdlolevel(LogLevel level) {
  switch (level) {
  case LogLevel::Trace:
    return spdlog::level::trace;
  case LogLevel::Debug:
    return spdlog::level::debug;
  case LogLevel::Info:
    return spdlog::level::info;
  case LogLevel::Warning:
    return spdlog::level::warn;
  case LogLevel::Error:
    return spdlog::level::err;
  case LogLevel::Critical:
    return spdlog::level::critical;
  default:
    return spdlog::level::info;
  }
}

void init_logging() {
  auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  console_sink->set_pattern("%^[%T] [%l] %v%$");
  logger = std::make_shared<spdlog::logger>("DY", console_sink);
  logger->set_level(spdlog::level::trace);
}

namespace detail {
void private_log_message(LogLevel level, std::string_view message) {
  std::call_once(init_flag, init_logging);
  logger->log(to_spdlolevel(level), message);
}
} // namespace detail
}