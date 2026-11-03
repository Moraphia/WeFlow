// Native NDJSON adapter for the public WeFlow weflow-export protocol.
// Basic text/raw export only; unsupported media requests fail explicitly.
#include "json.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>
using json = nlohmann::json;
namespace fs = std::filesystem;
extern "C" __declspec(dllimport) int32_t wcdb_init();
extern "C" __declspec(dllimport) int32_t wcdb_shutdown();
extern "C" __declspec(dllimport) int32_t wcdb_open_account(const char *,
                                                           const char *,
                                                           int64_t *);
extern "C" __declspec(dllimport) int32_t wcdb_set_my_account_id(int64_t,
                                                                const char *);
extern "C" __declspec(dllimport) int32_t wcdb_open_message_cursor(
    int64_t, const char *, int32_t, int32_t, int32_t, int32_t, int64_t *);
extern "C" __declspec(dllimport) int32_t wcdb_fetch_message_batch(int64_t,
                                                                  int64_t,
                                                                  void **,
                                                                  int32_t *);
extern "C" __declspec(dllimport) int32_t wcdb_close_message_cursor(int64_t,
                                                                   int64_t);
extern "C" __declspec(dllimport) void wcdb_free_string(void *);
static void emit(const json &event) {
  std::cout << event.dump(-1, ' ', false, json::error_handler_t::replace)
            << '\n'
            << std::flush;
}
static int32_t seconds(const json &value) {
  double number = value.is_number()   ? value.get<double>()
                  : value.is_string() ? std::stod(value.get<std::string>())
                                      : 0;
  if (number > 1e12)
    number /= 1000;
  return number > 0 && number < INT32_MAX ? static_cast<int32_t>(number) : 0;
}
static std::string sanitize(std::string input) {
  for (const auto &rule : std::vector<std::pair<std::string, std::string>>{
           {"1[3-9][0-9]{9}", "[PHONE]"},
           {"[0-9]{17}[0-9Xx]", "[ID]"},
           {"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}", "[EMAIL]"}})
    input = std::regex_replace(input, std::regex(rule.first), rule.second);
  return input;
}
int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--health") {
      emit({{"type", "result"},
            {"success", wcdb_init() == 0},
            {"provider", "independent-read-only"},
            {"formats", {"raw-jsonl"}},
            {"mediaExport", false}});
      wcdb_shutdown();
      return 0;
    }
    if (argc != 2 || std::string(argv[1]) != "weflow-export")
      throw std::runtime_error("Expected weflow-export command");
    auto started = std::chrono::steady_clock::now();
    std::string input((std::istreambuf_iterator<char>(std::cin)),
                      std::istreambuf_iterator<char>());
    if (input.empty() || input.size() > 1024 * 1024)
      throw std::runtime_error("Invalid export request");
    auto request = json::parse(input);
    if (request.contains("mediaTypes") && !request["mediaTypes"].empty())
      throw std::runtime_error(
          "This source-built engine currently exports text/raw messages. "
          "Disable image, voice, video, emoji and file export options.");
    if (request.value("format", std::string("raw-jsonl")) != "raw-jsonl")
      throw std::runtime_error("Only raw-jsonl is supported by the local "
                               "transport; WeFlow formats the final output");
    auto sessions = request.at("sessionIds").get<std::vector<std::string>>();
    auto account = request.at("account");
    auto sessionDb = account.at("sessionDb").get<std::string>();
    auto key = account.at("dbKey").get<std::string>();
    auto myId = account.value("myAccountId", std::string());
    auto directory = fs::u8path(request.value(
        "exportsDir", request.at("outputDir").get<std::string>()));
    fs::create_directories(directory);
    int64_t handle = 0;
    if (wcdb_init() != 0 ||
        wcdb_open_account(sessionDb.c_str(), key.c_str(), &handle) != 0)
      throw std::runtime_error(
          "Cannot open the selected encrypted account database");
    wcdb_set_my_account_id(handle, myId.c_str());
    emit({{"type", "ready"},
          {"total", sessions.size()},
          {"exports_dir", directory.u8string()}});
    json paths = json::object(), manifests = json::object(),
         failures = json::array();
    int success = 0;
    int batch = std::clamp(request.value("batchSize", 2000), 1, 20000);
    bool ascending = request.value("ascending", true);
    int32_t begin = 0, end = 0;
    if (request.contains("options") && request["options"].is_object() &&
        request["options"].contains("dateRange") &&
        request["options"]["dateRange"].is_object()) {
      const auto &range = request["options"]["dateRange"];
      if (range.contains("start"))
        begin = seconds(range["start"]);
      if (range.contains("end"))
        end = seconds(range["end"]);
    }
    auto nonce = std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    for (size_t index = 0; index < sessions.size(); ++index) {
      int64_t cursor = 0;
      auto output = directory / fs::u8path("session-" + nonce + "-" +
                                           std::to_string(index) + ".jsonl");
      auto partial = output;
      partial += ".partial";
      try {
        if (wcdb_open_message_cursor(handle, sessions[index].c_str(), batch,
                                     ascending ? 1 : 0, begin, end,
                                     &cursor) != 0)
          throw std::runtime_error("Cannot open message cursor");
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        if (!file)
          throw std::runtime_error("Cannot create the export file");
        uint64_t rows = 0;
        int32_t more = 1;
        while (more) {
          void *pointer = nullptr;
          if (wcdb_fetch_message_batch(handle, cursor, &pointer, &more) != 0 ||
              !pointer)
            throw std::runtime_error(
                "Message batch failed; export is incomplete");
          std::string serialized(static_cast<const char *>(pointer));
          wcdb_free_string(pointer);
          auto messages = json::parse(serialized);
          for (auto &message : messages) {
            if (request.value("sanitize", false))
              for (const auto &name : {"message_content", "compress_content"})
                if (message.contains(name) && message[name].is_string())
                  message[name] = sanitize(message[name].get<std::string>());
            file << message.dump(-1, ' ', false, json::error_handler_t::replace)
                 << '\n';
            if (!file)
              throw std::runtime_error("Writing export failed");
            ++rows;
          }
          emit({{"type", "progress"},
                {"phase", "export_messages"},
                {"session_id", sessions[index]},
                {"current", index},
                {"total", sessions.size()},
                {"exported_messages", rows}});
        }
        wcdb_close_message_cursor(handle, cursor);
        cursor = 0;
        file.close();
        if (!file)
          throw std::runtime_error("Closing the export file failed");
        fs::rename(partial, output);
        paths[sessions[index]] = output.u8string();
        manifests[sessions[index]] = {{"path", output.u8string()},
                                      {"rows", rows},
                                      {"bytes", fs::file_size(output)}};
        ++success;
        emit({{"type", "created_file"},
              {"session_id", sessions[index]},
              {"path", output.u8string()}});
      } catch (const std::exception &error) {
        if (cursor)
          wcdb_close_message_cursor(handle, cursor);
        std::error_code ignored;
        fs::remove(partial, ignored);
        failures.push_back(
            {{"session_id", sessions[index]}, {"error", error.what()}});
        emit({{"type", "session_error"},
              {"session_id", sessions[index]},
              {"error", error.what()}});
      }
    }
    wcdb_shutdown();
    emit({{"type", "result"},
          {"success", failures.empty()},
          {"success_count", success},
          {"fail_count", failures.size()},
          {"failed_sessions", failures},
          {"session_output_paths", paths},
          {"raw_session_output_paths", paths},
          {"raw_export_manifests", manifests},
          {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count()}});
    return failures.empty() ? 0 : 1;
  } catch (const std::exception &error) {
    wcdb_shutdown();
    emit({{"type", "result"}, {"success", false}, {"error", error.what()}});
    return 1;
  }
}
