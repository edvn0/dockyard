#include <dockyard/log.hpp>

#include <memory>
#include <mutex>
#include <spdlog/sinks/basic_file_sink.h>
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

  console_sink->set_level(spdlog::level::info);

  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
      "dockyard_profile.log", true);
  file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
  file_sink->set_level(
      spdlog::level::trace); // Capture absolutely everything here

  std::vector<spdlog::sink_ptr> sinks{console_sink, file_sink};
  logger = std::make_shared<spdlog::logger>("DY", sinks.begin(), sinks.end());

  logger->set_level(spdlog::level::trace);
  logger->flush_on(spdlog::level::trace);
}

namespace detail {
void private_log_message(LogLevel level, std::string_view message) {
  std::call_once(init_flag, init_logging);
  logger->log(to_spdlolevel(level), message);
}
} // namespace detail
} // namespace dy