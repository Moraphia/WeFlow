// Independent implementation of the public WeFlow provider ABI.
// Uses Tencent's open-source SQLCipher core. It does not load the expired
// wcdb_api.dll.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "json.hpp"
#include <algorithm>
#include <atomic>
#include <bcrypt.h>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <zstd/zstd.h>

using json = nlohmann::json;
namespace fs = std::filesystem;
#define API extern "C" __declspec(dllexport)
static std::recursive_mutex gate;
static std::vector<std::string> logs;
static int64_t nextHandle = 1;
static int64_t nextCursor = 1;

static std::string text(const char *value) { return value ? value : ""; }
static std::string quote(const std::string &value) {
  std::string result = "'";
  for (char c : value) {
    result += c;
    if (c == '\'')
      result += c;
  }
  return result + "'";
}
static std::string identifier(const std::string &value) {
  std::string result = "\"";
  for (char c : value) {
    result += c;
    if (c == '"')
      result += c;
  }
  return result + "\"";
}
static int output(const json &value, void **out) {
  if (!out)
    return -1;
  *out = nullptr;
  auto serialized = value.dump(-1, ' ', false, json::error_handler_t::replace);
  auto memory = static_cast<char *>(std::malloc(serialized.size() + 1));
  if (!memory)
    return -4;
  std::memcpy(memory, serialized.c_str(), serialized.size() + 1);
  *out = memory;
  return 0;
}
template <class Fn> static int guarded(Fn fn) {
  std::lock_guard<std::recursive_mutex> lock(gate);
  try {
    return fn();
  } catch (const std::exception &error) {
    logs.push_back(error.what());
    if (logs.size() > 80)
      logs.erase(logs.begin());
    return -4;
  } catch (...) {
    logs.push_back("Native provider operation failed");
    return -4;
  }
}
static std::string hex(const unsigned char *bytes, size_t size) {
  static const char *alphabet = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    result[2 * i] = alphabet[bytes[i] >> 4];
    result[2 * i + 1] = alphabet[bytes[i] & 15];
  }
  return result;
}
static std::string md5(const std::string &input) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0)
    throw std::runtime_error("MD5 initialization failed");
  unsigned char digest[16];
  auto status =
      BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char *>(input.data())),
                 static_cast<ULONG>(input.size()), digest, 16);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (status < 0)
    throw std::runtime_error("MD5 computation failed");
  return hex(digest, 16);
}
static json row(sqlite3_stmt *statement) {
  json result = json::object();
  for (int i = 0; i < sqlite3_column_count(statement); ++i) {
    std::string name = sqlite3_column_name(statement, i);
    switch (sqlite3_column_type(statement, i)) {
    case SQLITE_INTEGER: {
      int64_t value = sqlite3_column_int64(statement, i);
      if (name == "server_id" || name == "svr_id" || value > 9007199254740991LL ||
          value < -9007199254740991LL)
        result[name] = std::to_string(value);
      else
        result[name] = value;
      break;
    }
    case SQLITE_FLOAT:
      result[name] = sqlite3_column_double(statement, i);
      break;
    case SQLITE_TEXT:
      result[name] = std::string(reinterpret_cast<const char *>(sqlite3_column_text(statement, i)),
                                 sqlite3_column_bytes(statement, i));
      break;
    case SQLITE_BLOB: {
      auto bytes = static_cast<const unsigned char *>(sqlite3_column_blob(statement, i));
      auto length = static_cast<size_t>(sqlite3_column_bytes(statement, i));
      bool content = name == "message_content" || name == "compress_content" || name == "source";
      if (content && length >= 4 && bytes[0] == 0x28 && bytes[1] == 0xb5 && bytes[2] == 0x2f &&
          bytes[3] == 0xfd) {
        size_t capacity = std::max<size_t>(4096, length * 4);
        for (;;) {
          if (capacity > 64 * 1024 * 1024)
            throw std::runtime_error("Compressed message exceeds supported size");
          std::string decoded(capacity, '\0');
          auto size = ZSTD_decompress(decoded.data(), capacity, bytes, length);
          if (!ZSTD_isError(size)) {
            decoded.resize(size);
            result[name] = decoded;
            break;
          }
          if (std::string(ZSTD_getErrorName(size)).find("Destination buffer is too small") ==
              std::string::npos)
            throw std::runtime_error("Compressed message cannot be decoded without its dictionary");
          capacity *= 2;
        }
      } else if (content && length &&
                 MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     reinterpret_cast<const char *>(bytes),
                                     static_cast<int>(length), nullptr, 0) > 0) {
        result[name] = std::string(reinterpret_cast<const char *>(bytes), length);
      } else
        result[name] = hex(bytes, length);
      break;
    }
    default:
      result[name] = nullptr;
    }
  }
  return result;
}
static json query(sqlite3 *db, const std::string &sql) {
  sqlite3_stmt *raw = nullptr;
  const char *tail = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, &tail) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
  if (!raw || !sqlite3_stmt_readonly(raw))
    throw std::runtime_error("Only read-only queries are supported");
  while (tail && *tail && std::isspace(static_cast<unsigned char>(*tail)))
    ++tail;
  if (tail && *tail)
    throw std::runtime_error("Multiple statements are not supported");
  json result = json::array();
  int status;
  while ((status = sqlite3_step(raw)) == SQLITE_ROW) {
    if (result.size() >= 1000000)
      throw std::runtime_error("Query exceeds row limit; use a message cursor");
    result.push_back(row(raw));
  }
  if (status != SQLITE_DONE)
    throw std::runtime_error(sqlite3_errmsg(db));
  return result;
}
static bool hasTable(sqlite3 *db, const std::string &table) {
  return !query(db, "SELECT name FROM sqlite_master WHERE type='table' AND name=" + quote(table) +
                        " LIMIT 1")
              .empty();
}
static sqlite3 *openReadOnly(const fs::path &path, const std::string &key, bool writable = false,
                             bool createMissing = false) {
  if (!fs::is_regular_file(path) && !(writable && createMissing))
    throw std::runtime_error("Database file was not found: " + path.filename().u8string());
  if (!std::regex_match(key, std::regex("[a-fA-F0-9]{64}([a-fA-F0-9]{32})?")))
    throw std::runtime_error("Expected a 32-byte hexadecimal database key");
  for (int mode = 0; mode < 4; ++mode) {
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.u8string().c_str(), &db,
                        (writable
                             ? (SQLITE_OPEN_READWRITE | (createMissing ? SQLITE_OPEN_CREATE : 0))
                             : SQLITE_OPEN_READONLY) |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      if (db)
        sqlite3_close(db);
      continue;
    }
    sqlite3_busy_timeout(db, 3000);
    std::string material;
    if (mode % 2 == 0)
      material = "x'" + key + "'";
    else
      for (size_t i = 0; i < key.size(); i += 2)
        material.push_back(static_cast<char>(std::stoi(key.substr(i, 2), nullptr, 16)));
    sqlite3_key(db, material.data(), static_cast<int>(material.size()));
    sqlite3_exec(db,
                 mode < 2 ? "PRAGMA cipher_compatibility=4; PRAGMA cipher_page_size=4096;"
                          : "PRAGMA cipher_compatibility=3; PRAGMA cipher_page_size=4096;",
                 nullptr, nullptr, nullptr);
    if (!writable)
      sqlite3_exec(db, "PRAGMA query_only=ON;", nullptr, nullptr, nullptr);
    try {
      query(db, "SELECT count(*) AS count FROM sqlite_master");
      return db;
    } catch (...) {
      sqlite3_close(db);
    }
  }
  throw std::runtime_error("Unable to read database with the supplied key or cipher settings: " +
                           path.filename().u8string());
}
struct Account {
  fs::path session;
  fs::path storage;
  std::string key;
  std::string myId;
  std::map<std::string, sqlite3 *> databases;
  ~Account() {
    for (auto &item : databases)
      sqlite3_close_v2(item.second);
  }
  sqlite3 *database(const fs::path &path) {
    auto normalized = fs::weakly_canonical(path).u8string();
    auto found = databases.find(normalized);
    if (found != databases.end())
      return found->second;
    return databases.emplace(normalized, openReadOnly(path, key)).first->second;
  }
  std::vector<fs::path> files(const std::string &kind) {
    if (kind.empty() || kind == "session")
      return {session};
    std::vector<fs::path> result;
    for (const auto &item :
         fs::recursive_directory_iterator(storage, fs::directory_options::skip_permission_denied)) {
      if (!item.is_regular_file())
        continue;
      auto name = item.path().filename().u8string();
      bool matches =
          kind == "message"
              ? std::regex_match(name, std::regex("message_[0-9]+\\.db", std::regex::icase))
          : kind == "media"
              ? std::regex_match(name, std::regex("media_[0-9]+\\.db", std::regex::icase))
              : name == kind + ".db";
      if (matches)
        result.push_back(item.path());
    }
    std::sort(result.begin(), result.end());
    return result;
  }
  fs::path resolve(const std::string &kind, const std::string &explicitPath) {
    if (!explicitPath.empty()) {
      auto candidate = fs::weakly_canonical(fs::u8path(explicitPath));
      auto relative = fs::relative(candidate, fs::weakly_canonical(storage));
      if (relative.empty() || *relative.begin() == "..")
        throw std::runtime_error("Database path is outside the selected account");
      return candidate;
    }
    auto candidates = files(kind);
    if (candidates.empty())
      throw std::runtime_error("No database available for kind: " + kind);
    return candidates.front();
  }
};
struct Stream {
  sqlite3_stmt *statement = nullptr;
  fs::path path;
  json current;
  bool done = false;
};
struct Cursor {
  std::shared_ptr<Account> account;
  std::vector<Stream> streams;
  bool ascending = false;
  int batch = 500;
  std::string table;
  ~Cursor() {
    for (auto &stream : streams)
      if (stream.statement)
        sqlite3_finalize(stream.statement);
  }
};
static std::map<int64_t, std::shared_ptr<Account>> accounts;
static std::map<int64_t, std::unique_ptr<Cursor>> cursors;
static std::shared_ptr<Account> account(int64_t handle) {
  auto found = accounts.find(handle);
  if (found == accounts.end())
    throw std::runtime_error("Invalid account handle");
  return found->second;
}
static int64_t number(const json &object, const std::string &key) {
  auto found = object.find(key);
  if (found == object.end() || found->is_null())
    return 0;
  try {
    return found->is_string() ? std::stoll(found->get<std::string>()) : found->get<int64_t>();
  } catch (...) {
    return 0;
  }
}
static void advance(Stream &stream, const Account &owner, const std::string &table) {
  int status = sqlite3_step(stream.statement);
  if (status == SQLITE_DONE) {
    stream.done = true;
    stream.current = nullptr;
    return;
  }
  if (status != SQLITE_ROW)
    throw std::runtime_error("Message cursor read failed");
  stream.current = row(stream.statement);
  stream.current["_db_path"] = stream.path.u8string();
  stream.current["_table_name"] = table;
  auto sender = stream.current.value("sender_username", std::string());
  if (!owner.myId.empty() && !sender.empty())
    stream.current["computed_is_send"] = sender == owner.myId ? 1 : 0;
}
static std::unique_ptr<Cursor> newCursor(std::shared_ptr<Account> owner, const std::string &session,
                                         int batch, bool ascending, int begin, int end,
                                         const std::string &extra = "") {
  if (session.empty())
    throw std::runtime_error("Session ID is required");
  auto cursor = std::make_unique<Cursor>();
  cursor->account = owner;
  cursor->batch = std::clamp(batch, 1, 20000);
  cursor->ascending = ascending;
  cursor->table = "Msg_" + md5(session);
  for (const auto &file : owner->files("message")) {
    auto db = owner->database(file);
    if (!hasTable(db, cursor->table))
      continue;
    bool names = hasTable(db, "Name2Id");
    std::string sql = "SELECT m.*, " + std::string(names ? "COALESCE(n.user_name,'')" : "''") +
                      " AS sender_username FROM " + identifier(cursor->table) + " AS m ";
    if (names)
      sql += "LEFT JOIN Name2Id AS n ON n.rowid=m.real_sender_id ";
    sql += "WHERE 1=1 ";
    if (begin > 0)
      sql += "AND m.create_time >= " + std::to_string(begin) + " ";
    if (end > 0)
      sql += "AND m.create_time <= " + std::to_string(end) + " ";
    sql += extra;
    auto direction = ascending ? " ASC" : " DESC";
    sql += " ORDER BY m.sort_seq" + std::string(direction) + ",m.create_time" + direction +
           ",m.local_id" + direction;
    Stream stream;
    stream.path = file;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stream.statement, nullptr) != SQLITE_OK)
      throw std::runtime_error("Message schema mismatch: " + std::string(sqlite3_errmsg(db)));
    cursor->streams.push_back(std::move(stream));
    advance(cursor->streams.back(), *owner, cursor->table);
  }
  return cursor;
}
static bool earlier(const Stream &a, const Stream &b, bool ascending) {
  for (const std::string key : {"sort_seq", "create_time", "local_id"}) {
    auto x = number(a.current, key), y = number(b.current, key);
    if (x != y)
      return ascending ? x < y : x > y;
  }
  return a.path.u8string() < b.path.u8string();
}
static bool more(const Cursor &cursor) {
  return std::any_of(cursor.streams.begin(), cursor.streams.end(),
                     [](const Stream &s) { return !s.done; });
}
static json fetch(Cursor &cursor, int maximum) {
  json rows = json::array();
  while (static_cast<int>(rows.size()) < maximum) {
    Stream *selected = nullptr;
    for (auto &stream : cursor.streams)
      if (!stream.done && (!selected || earlier(stream, *selected, cursor.ascending)))
        selected = &stream;
    if (!selected)
      break;
    rows.push_back(selected->current);
    advance(*selected, *cursor.account, cursor.table);
  }
  return rows;
}

API int32_t wcdb_init() { return 0; }
API void wcdb_free_string(void *pointer) { std::free(pointer); }
static void stopMonitor();
API int32_t wcdb_shutdown() {
  stopMonitor();
  return guarded([]() {
    cursors.clear();
    accounts.clear();
    return 0;
  });
}
API int32_t wcdb_open_account(const char *sessionPath, const char *key, int64_t *handle) {
  return guarded([&]() {
    if (!handle)
      return -1;
    *handle = 0;
    auto value = std::make_shared<Account>();
    value->session = fs::weakly_canonical(fs::u8path(text(sessionPath)));
    value->key = text(key);
    value->storage = value->session.parent_path();
    for (auto p = value->storage; !p.empty() && p != p.root_path(); p = p.parent_path())
      if (p.filename() == "db_storage") {
        value->storage = p;
        break;
      }
    auto db = value->database(value->session);
    if (!hasTable(db, "SessionTable"))
      throw std::runtime_error("SessionTable is missing from the selected database");
    *handle = nextHandle++;
    accounts[*handle] = value;
    return 0;
  });
}
API int32_t wcdb_close_account(int64_t handle) {
  return guarded([&]() {
    auto owner = account(handle);
    for (auto it = cursors.begin(); it != cursors.end();) {
      if (it->second->account == owner)
        it = cursors.erase(it);
      else
        ++it;
    }
    accounts.erase(handle);
    return 0;
  });
}
API int32_t wcdb_set_my_account_id(int64_t handle, const char *id) {
  return guarded([&]() {
    auto value = text(id);
    if (value.rfind("wxid_", 0) == 0) {
      auto suffix = value.find('_', 5);
      if (suffix != std::string::npos)
        value.resize(suffix);
    }
    account(handle)->myId = value;
    return 0;
  });
}
API int32_t wcdb_get_sessions(int64_t handle, void **out) {
  return guarded([&]() {
    auto a = account(handle);
    return output(query(a->database(a->session),
                        "SELECT * FROM SessionTable ORDER BY sort_timestamp "
                        "DESC,last_timestamp DESC"),
                  out);
  });
}
API int32_t wcdb_exec_query(int64_t handle, const char *kind, const char *dbPath, const char *sql,
                            void **out) {
  return guarded([&]() {
    auto a = account(handle);
    return output(query(a->database(a->resolve(text(kind), text(dbPath))), text(sql)), out);
  });
}
API int32_t wcdb_get_contact(int64_t handle, const char *username, void **out) {
  return guarded([&]() {
    auto a = account(handle);
    auto db = a->database(a->resolve("contact", ""));
    auto rows =
        query(db, "SELECT * FROM contact WHERE username=" + quote(text(username)) + " LIMIT 1");
    return output(rows.empty() ? json(nullptr) : rows[0], out);
  });
}
API int32_t wcdb_get_contacts_compact(int64_t handle, const char *usersJson, void **out) {
  return guarded([&]() {
    auto a = account(handle);
    auto sql = std::string("SELECT * FROM contact");
    auto users = json::parse(text(usersJson).empty() ? "[]" : text(usersJson));
    if (!users.empty()) {
      sql += " WHERE username IN (";
      bool first = true;
      for (const auto &id : users) {
        if (!first)
          sql += ",";
        first = false;
        sql += quote(id.get<std::string>());
      }
      sql += ")";
    }
    return output(query(a->database(a->resolve("contact", "")), sql), out);
  });
}
API int32_t wcdb_get_display_names(int64_t handle, const char *usersJson, void **out) {
  return guarded([&]() {
    auto a = account(handle);
    auto db = a->database(a->resolve("contact", ""));
    json result = json::object();
    for (const auto &id : json::parse(text(usersJson))) {
      auto username = id.get<std::string>();
      auto rows = query(db, "SELECT username,remark,nick_name FROM contact WHERE username=" +
                                quote(username) + " LIMIT 1");
      if (!rows.empty()) {
        auto name =
            rows[0]["remark"].is_string() ? rows[0]["remark"].get<std::string>() : std::string();
        if (name.empty())
          name =
              rows[0]["nick_name"].is_string() ? rows[0]["nick_name"].get<std::string>() : username;
        result[username] = name;
      }
    }
    return output(result, out);
  });
}
API int32_t wcdb_get_messages(int64_t handle, const char *session, int32_t limit, int32_t offset,
                              void **out) {
  return guarded([&]() {
    auto cursor = newCursor(account(handle), text(session), limit, false, 0, 0);
    int skip = std::max(0, offset);
    while (skip > 0 && more(*cursor)) {
      int count = std::min(skip, 20000);
      auto discarded = fetch(*cursor, count);
      skip -= static_cast<int>(discarded.size());
    }
    return output(fetch(*cursor, std::clamp(limit, 1, 20000)), out);
  });
}
API int32_t wcdb_get_messages_by_type(int64_t handle, const char *session, int64_t type,
                                      int32_t ascending, int32_t limit, int32_t offset,
                                      void **out) {
  return guarded([&]() {
    auto cursor = newCursor(account(handle), text(session), limit, ascending != 0, 0, 0,
                            "AND m.local_type=" + std::to_string(type));
    int skip = std::max(0, offset);
    while (skip > 0 && more(*cursor)) {
      auto discarded = fetch(*cursor, std::min(skip, 20000));
      skip -= static_cast<int>(discarded.size());
    }
    return output(fetch(*cursor, std::clamp(limit, 1, 20000)), out);
  });
}
API int32_t wcdb_get_message_count(int64_t handle, const char *session, int32_t *out) {
  return guarded([&]() {
    if (!out)
      return -1;
    auto a = account(handle);
    auto table = "Msg_" + md5(text(session));
    int64_t count = 0;
    for (const auto &path : a->files("message")) {
      auto db = a->database(path);
      if (hasTable(db, table))
        count +=
            number(query(db, "SELECT count(*) AS count FROM " + identifier(table))[0], "count");
    }
    if (count > INT32_MAX)
      throw std::runtime_error("Message count exceeds ABI limit");
    *out = static_cast<int32_t>(count);
    return 0;
  });
}
API int32_t wcdb_get_session_message_counts(int64_t handle, const char *sessionsJson, void **out) {
  return guarded([&]() {
    json result = json::object();
    for (const auto &session : json::parse(text(sessionsJson))) {
      int32_t count = 0;
      int status = wcdb_get_message_count(handle, session.get<std::string>().c_str(), &count);
      if (status)
        return status;
      result[session.get<std::string>()] = count;
    }
    return output(result, out);
  });
}
API int32_t wcdb_get_message_by_id(int64_t handle, const char *session, int32_t id, void **out) {
  return guarded([&]() {
    auto cursor = newCursor(account(handle), text(session), 1, false, 0, 0,
                            "AND m.local_id=" + std::to_string(id));
    auto rows = fetch(*cursor, 1);
    return output(rows.empty() ? json(nullptr) : rows[0], out);
  });
}
API int32_t wcdb_get_message_by_svrid(int64_t handle, const char *session, const char *id,
                                      void **out) {
  return guarded([&]() {
    auto value = text(id);
    if (!std::regex_match(value, std::regex("[0-9]+")))
      return -1;
    auto cursor =
        newCursor(account(handle), text(session), 1, false, 0, 0, "AND m.server_id=" + value);
    auto rows = fetch(*cursor, 1);
    return output(rows.empty() ? json(nullptr) : rows[0], out);
  });
}
API int32_t wcdb_open_message_cursor(int64_t handle, const char *session, int32_t batch,
                                     int32_t ascending, int32_t begin, int32_t end, int64_t *out) {
  return guarded([&]() {
    if (!out)
      return -1;
    *out = 0;
    auto cursor = newCursor(account(handle), text(session), batch, ascending != 0, begin, end);
    *out = nextCursor++;
    cursors[*out] = std::move(cursor);
    return 0;
  });
}
API int32_t wcdb_fetch_message_batch(int64_t handle, int64_t cursorId, void **out,
                                     int32_t *hasMore) {
  return guarded([&]() {
    auto found = cursors.find(cursorId);
    if (found == cursors.end() || found->second->account != account(handle) || !hasMore)
      return -1;
    auto rows = fetch(*found->second, found->second->batch);
    *hasMore = more(*found->second) ? 1 : 0;
    return output(rows, out);
  });
}
API int32_t wcdb_close_message_cursor(int64_t handle, int64_t cursorId) {
  return guarded([&]() {
    auto found = cursors.find(cursorId);
    if (found == cursors.end() || found->second->account != account(handle))
      return -1;
    cursors.erase(found);
    return 0;
  });
}
API int32_t wcdb_list_message_dbs(int64_t handle, void **out) {
  return guarded([&]() {
    json paths = json::array();
    for (const auto &p : account(handle)->files("message"))
      paths.push_back(p.u8string());
    return output(paths, out);
  });
}
API int32_t wcdb_list_tables(int64_t handle, const char *kind, const char *dbPath, void **out) {
  return guarded([&]() {
    auto a = account(handle);
    auto rows = query(a->database(a->resolve(text(kind), text(dbPath))),
                      "SELECT name FROM sqlite_master WHERE type IN "
                      "('table','view') ORDER BY name");
    json names = json::array();
    for (auto &r : rows)
      names.push_back(r["name"]);
    return output(names, out);
  });
}
API int32_t wcdb_get_table_schema(int64_t handle, const char *kind, const char *dbPath,
                                  const char *table, void **out) {
  return guarded([&]() {
    auto a = account(handle);
    auto rows = query(a->database(a->resolve(text(kind), text(dbPath))),
                      "SELECT sql FROM sqlite_master WHERE name=" + quote(text(table)));
    return output({{"success", true}, {"schema", rows.empty() ? json("") : rows[0]["sql"]}}, out);
  });
}
API int32_t wcdb_get_logs(void **out) {
  return guarded([&]() { return output(logs, out); });
}
API int32_t wcdb_get_db_status(int64_t handle, void **out) {
  return guarded([&]() {
    auto a = account(handle);
    return output({{"connected", true},
                   {"provider", "independent-read-only"},
                   {"message_db_count", a->files("message").size()}},
                  out);
  });
}

#include "features.inc"
