// Poll database/WAL metadata only. Shared-memory changes from our own readers
// do not constitute new messages. The pipe is private to this provider process.
static std::mutex monitorGate;
static std::atomic<bool> monitorRunning{false};
static std::thread *monitorThread = nullptr;
static std::string monitorName() {
  return "\\\\.\\pipe\\weflow_local_monitor_" +
         std::to_string(GetCurrentProcessId());
}
static void stopMonitor() {
  std::lock_guard<std::mutex> lock(monitorGate);
  monitorRunning = false;
  if (monitorThread) {
    monitorThread->join();
    delete monitorThread;
    monitorThread = nullptr;
  }
}
API void wcdb_stop_monitor_pipe() { stopMonitor(); }
API int32_t wcdb_get_monitor_pipe_name(void **out) {
  return rawOutput(monitorName(), out);
}
API int32_t wcdb_start_monitor_pipe() {
  std::lock_guard<std::mutex> lock(monitorGate);
  if (monitorRunning)
    return 0;
  auto pipe =
      CreateNamedPipeA(monitorName().c_str(), PIPE_ACCESS_DUPLEX,
                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1,
                       65536, 65536, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE)
    return -4;
  monitorRunning = true;
  monitorThread = new std::thread([pipe]() {
    std::map<std::string, std::pair<uintmax_t, fs::file_time_type>> stamps;
    bool connected = false;
    while (monitorRunning) {
      if (!connected) {
        BOOL ok = ConnectNamedPipe(pipe, nullptr);
        connected = ok || GetLastError() == ERROR_PIPE_CONNECTED;
      }
      std::vector<std::string> events;
      try {
        std::lock_guard<std::recursive_mutex> lock(gate);
        std::set<std::string> active;
        for (auto &pair : accounts) {
          auto a = pair.second;
          for (auto &kind : {"session", "message", "contact", "sns"})
            for (auto &db : a->files(kind))
              for (auto suffix : {"", "-wal"}) {
                auto file = fs::u8path(db.u8string() + suffix);
                auto name = file.u8string();
                active.insert(name);
                std::error_code ec;
                if (!fs::is_regular_file(file, ec))
                  continue;
                auto stamp = std::make_pair(fs::file_size(file, ec),
                                            fs::last_write_time(file, ec));
                auto previous = stamps.find(name);
                if (previous != stamps.end() && previous->second != stamp) {
                  json event = {{"action", "update"},
                                {"kind", kind},
                                {"db_path", db.u8string()}};
                  if (std::string(kind) == "session")
                    event["table"] = "SessionTable";
                  if (std::string(kind) == "message") {
                    json tables = json::array();
                    for (auto &t : query(a->database(db),
                                         "SELECT name FROM sqlite_master WHERE "
                                         "type='table' AND name LIKE 'Msg_%'"))
                      tables.push_back(t["name"]);
                    event["tables"] = tables;
                  }
                  events.push_back(event.dump() + "\n");
                }
                stamps[name] = stamp;
              }
        }
        for (auto i = stamps.begin(); i != stamps.end();)
          if (!active.count(i->first))
            i = stamps.erase(i);
          else
            ++i;
      } catch (...) {
      }
      if (connected)
        for (auto &event : events) {
          DWORD written = 0;
          if (!WriteFile(pipe, event.data(), DWORD(event.size()), &written,
                         nullptr) ||
              written != event.size()) {
            DisconnectNamedPipe(pipe);
            connected = false;
            break;
          }
        }
      for (int i = 0; i < 5 && monitorRunning; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
  });
  return 0;
}
