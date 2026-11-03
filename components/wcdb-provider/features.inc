// Additional independently implemented provider features.
static std::string str(const json &object, const std::string &key,
                       const std::string &fallback = "") {
  auto i = object.find(key);
  return i != object.end() && i->is_string() ? i->get<std::string>() : fallback;
}
static int rawOutput(const std::string &value, void **out) {
  if (!out)
    return -1;
  *out = std::malloc(value.size() + 1);
  if (!*out)
    return -4;
  std::memcpy(*out, value.c_str(), value.size() + 1);
  return 0;
}
static std::string unhex(const std::string &value) {
  if (value.size() % 2 || !std::regex_match(value, std::regex("[0-9a-fA-F]*")))
    return {};
  std::string bytes;
  for (size_t i = 0; i < value.size(); i += 2)
    bytes += static_cast<char>(std::stoi(value.substr(i, 2), nullptr, 16));
  return bytes;
}
static json tableColumns(sqlite3 *db, const std::string &table) {
  return query(db, "PRAGMA table_info(" + identifier(table) + ")");
}
static std::set<std::string> columnNames(sqlite3 *db, const std::string &table) {
  std::set<std::string> result;
  for (auto &r : tableColumns(db, table))
    result.insert(str(r, "name"));
  return result;
}
static json sessionIds(std::shared_ptr<Account> a) {
  json result = json::array();
  for (auto &r : query(a->database(a->session), "SELECT username FROM SessionTable"))
    result.push_back(r["username"]);
  return result;
}
static void eachMessage(std::shared_ptr<Account> a, const json &ids, int begin, int end,
                        const std::function<void(const std::string &, const json &)> &callback) {
  for (auto &id : ids) {
    auto sid = id.get<std::string>();
    auto c = newCursor(a, sid, 2000, true, begin, end);
    while (more(*c))
      for (auto &r : fetch(*c, 2000))
        callback(sid, r);
  }
}
static std::tm local(int64_t seconds) {
  std::tm tm{};
  time_t t = seconds;
  localtime_s(&tm, &t);
  return tm;
}
static std::string dateKey(int64_t seconds, const char *format = "%Y-%m-%d") {
  auto tm = local(seconds);
  char buffer[64];
  std::strftime(buffer, sizeof buffer, format, &tm);
  return buffer;
}
static size_t charCount(const std::string &s) {
  size_t n = 0;
  for (unsigned char c : s)
    if ((c & 0xc0) != 0x80)
      ++n;
  return n;
}
static json dateCounts(std::shared_ptr<Account> a, const std::string &sid) {
  json counts = json::object();
  eachMessage(a, json::array({sid}), 0, 0, [&](auto &, auto &r) {
    auto key = dateKey(number(r, "create_time"));
    counts[key] = number(counts, key) + 1;
  });
  return counts;
}
static json contactRows(std::shared_ptr<Account> a) {
  auto paths = a->files("contact");
  if (paths.empty())
    return json::array();
  return query(a->database(paths.front()), "SELECT * FROM contact");
}
static json contactMap(std::shared_ptr<Account> a, const json &ids, const std::string &field) {
  auto db = a->database(a->resolve("contact", ""));
  json map = json::object();
  for (auto &id : ids) {
    auto name = id.get<std::string>();
    auto r = query(db, "SELECT * FROM contact WHERE username=" + quote(name) + " LIMIT 1");
    if (!r.empty())
      map[name] = str(r[0], field);
  }
  return map;
}
static json messageTables(std::shared_ptr<Account> a, const std::string &sid) {
  json result = json::array();
  auto table = "Msg_" + md5(sid);
  for (auto &p : a->files("message")) {
    auto db = a->database(p);
    if (!hasTable(db, table))
      continue;
    auto stats = query(db, "SELECT COUNT(*) count,MIN(create_time) "
                           "min_time,MAX(create_time) max_time FROM " +
                               identifier(table))[0];
    stats["db_path"] = p.u8string();
    stats["table_name"] = table;
    stats["tableName"] = table;
    stats["name"] = table;
    result.push_back(stats);
  }
  return result;
}

API int32_t wcdb_purge_memory() {
  return guarded([]() {
    for (auto &a : accounts)
      for (auto &db : a.second->databases)
        sqlite3_db_release_memory(db.second);
    return 0;
  });
}
API int32_t wcdb_set_message_cursor_projection(int64_t h, int64_t c, int32_t projection) {
  return guarded([&]() {
    auto i = cursors.find(c);
    return i != cursors.end() && i->second->account == account(h) &&
                   (projection == 0 || projection == 1)
               ? 0
               : -1;
  });
}
API int32_t wcdb_get_message_tables(int64_t h, const char *sid, void **out) {
  return guarded([&]() { return output(messageTables(account(h), text(sid)), out); });
}
API int32_t wcdb_get_message_table_stats(int64_t h, const char *sid, void **out) {
  return wcdb_get_message_tables(h, sid, out);
}
API int32_t wcdb_get_message_dates(int64_t h, const char *sid, void **out) {
  return guarded([&]() {
    auto counts = dateCounts(account(h), text(sid));
    json dates = json::array();
    for (auto i = counts.begin(); i != counts.end(); ++i)
      dates.push_back(i.key());
    return output(dates, out);
  });
}
API int32_t wcdb_get_session_message_date_counts(int64_t h, const char *sid, void **out) {
  return guarded([&]() { return output(dateCounts(account(h), text(sid)), out); });
}
API int32_t wcdb_get_session_message_date_counts_batch(int64_t h, const char *ids, void **out) {
  return guarded([&]() {
    json r = json::object();
    for (auto &id : json::parse(text(ids)))
      r[id.get<std::string>()] = dateCounts(account(h), id.get<std::string>());
    return output(r, out);
  });
}
API int32_t wcdb_get_message_meta(int64_t h, const char *dbPath, const char *table, int32_t limit,
                                  int32_t offset, void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto p = a->resolve("message", text(dbPath));
    return output(
        query(a->database(p), "SELECT * FROM " + identifier(text(table)) + " LIMIT " +
                                  std::to_string(limit > 0 ? std::min(limit, 20000) : 20000) +
                                  " OFFSET " + std::to_string(std::max(0, offset))),
        out);
  });
}
API int32_t wcdb_get_message_table_columns(int64_t h, const char *dbPath, const char *table,
                                           void **out) {
  return guarded([&]() {
    auto a = account(h);
    json columns = json::array();
    for (auto &r : tableColumns(a->database(a->resolve("message", text(dbPath))), text(table)))
      columns.push_back(r["name"]);
    return output(columns, out);
  });
}
API int32_t wcdb_get_message_table_time_range(int64_t h, const char *dbPath, const char *table,
                                              void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto rows = query(a->database(a->resolve("message", text(dbPath))),
                      "SELECT MIN(create_time) AS min_time,MAX(create_time) AS "
                      "max_time FROM " +
                          identifier(text(table)));
    return output(rows[0], out);
  });
}
API int32_t wcdb_search_messages(int64_t h, const char *sid, const char *keyword, int32_t limit,
                                 int32_t offset, int32_t begin, int32_t end, void **out) {
  return guarded([&]() {
    auto a = account(h);
    json results = json::array();
    auto ids = text(sid).empty() ? sessionIds(a) : json::array({text(sid)});
    auto term = text(keyword);
    eachMessage(a, ids, begin, end, [&](const auto &session, const auto &r) {
      if (term.empty() || str(r, "message_content").find(term) != std::string::npos ||
          str(r, "compress_content").find(term) != std::string::npos) {
        auto item = r;
        item["session_id"] = session;
        results.push_back(item);
      }
    });
    std::sort(results.begin(), results.end(),
              [](auto &x, auto &y) { return number(x, "create_time") > number(y, "create_time"); });
    json page = json::array();
    size_t first = std::min(results.size(), static_cast<size_t>(std::max(0, offset)));
    size_t last = std::min(results.size(), first + static_cast<size_t>(limit > 0 ? limit : 100));
    for (size_t i = first; i < last; ++i)
      page.push_back(results[i]);
    return output(page, out);
  });
}
API int32_t wcdb_get_contact_alias_map(int64_t h, const char *ids, void **out) {
  return guarded(
      [&]() { return output(contactMap(account(h), json::parse(text(ids)), "alias"), out); });
}
API int32_t wcdb_get_avatar_urls(int64_t h, const char *ids, void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto large = contactMap(a, json::parse(text(ids)), "big_head_url"),
         small = contactMap(a, json::parse(text(ids)), "small_head_url");
    for (auto i = large.begin(); i != large.end(); ++i)
      if (i.value() == "")
        i.value() = small.value(i.key(), json(""));
    return output(large, out);
  });
}
API int32_t wcdb_get_contact_friend_flags(int64_t h, const char *ids, void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto db = a->database(a->resolve("contact", ""));
    json flags = json::object();
    for (auto &id : json::parse(text(ids))) {
      auto username = id.get<std::string>();
      auto rows = query(db, "SELECT * FROM contact WHERE username=" + quote(username) + " LIMIT 1");
      if (!rows.empty())
        flags[username] = number(rows[0], "local_type") == 1 &&
                          number(rows[0], "delete_flag") == 0 && (number(rows[0], "flag") & 8) == 0;
    }
    return output(flags, out);
  });
}
API int32_t wcdb_get_contact_type_counts(int64_t h, void **out) {
  return guarded([&]() {
    json c = {{"private", 0}, {"group", 0}, {"official", 0}, {"former_friend", 0}, {"blocked", 0}};
    for (auto &r : contactRows(account(h))) {
      auto u = str(r, "username");
      auto type = number(r, "local_type"), flag = number(r, "flag");
      bool official = u.rfind("gh_", 0) == 0;
      bool former = !official && ((type == 0 && !str(r, "quan_pin").empty()) ||
                                  (type == 3 && flag != 4 &&
                                   (!str(r, "quan_pin").empty() || !str(r, "alias").empty() ||
                                    !str(r, "remark").empty())));
      const std::set<std::string> excluded{"medianote", "floatbottle", "qmessage", "qqmail",
                                           "fmessage"};
      std::string category;
      if (u.size() >= 9 && u.substr(u.size() - 9) == "@chatroom")
        category = "group";
      else if (official && type == 1 && u != "gh_f0a92aa7146c")
        category = "official";
      else if (!official && (flag & 8))
        category = "blocked";
      else if (former)
        category = "former_friend";
      else if (type == 1 && !(flag & 8) && !excluded.count(u))
        category = "private";
      if (!category.empty())
        c[category] = number(c, category) + 1;
    }
    return output(c, out);
  });
}

struct ProtoField {
  unsigned field;
  unsigned wire;
  uint64_t integer = 0;
  std::string bytes;
};
static std::vector<ProtoField> proto(const std::string &input) {
  std::vector<ProtoField> result;
  size_t offset = 0;
  auto varint = [&]() {
    uint64_t n = 0;
    for (unsigned shift = 0; shift < 64 && offset < input.size(); shift += 7) {
      unsigned b = static_cast<unsigned char>(input[offset++]);
      n |= uint64_t(b & 127) << shift;
      if (!(b & 128))
        return n;
    }
    throw std::runtime_error("Malformed protobuf data");
  };
  try {
    while (offset < input.size()) {
      auto tag = varint();
      ProtoField f;
      f.field = unsigned(tag >> 3);
      f.wire = unsigned(tag & 7);
      if (!f.field)
        break;
      if (f.wire == 0)
        f.integer = varint();
      else if (f.wire == 1) {
        if (offset + 8 > input.size())
          break;
        offset += 8;
      } else if (f.wire == 5) {
        if (offset + 4 > input.size())
          break;
        offset += 4;
      } else if (f.wire == 2) {
        auto n = varint();
        if (n > input.size() - offset)
          break;
        f.bytes = input.substr(offset, size_t(n));
        offset += size_t(n);
      } else
        break;
      result.push_back(std::move(f));
    }
  } catch (...) {
  }
  return result;
}
static json groupMembers(std::shared_ptr<Account> a, const std::string &room) {
  auto db = a->database(a->resolve("contact", ""));
  if (!hasTable(db, "chatroom_member") || !hasTable(db, "name2id"))
    return json::array();
  return query(db, "SELECT n.username,c.*,n.username AS username FROM "
                   "chatroom_member AS m JOIN name2id AS r ON r.rowid=m.room_id "
                   "JOIN name2id AS n ON n.rowid=m.member_id LEFT JOIN contact AS "
                   "c ON c.username=n.username WHERE r.username=" +
                       quote(room));
}
static json groupNicknames(std::shared_ptr<Account> a, const std::string &room) {
  auto db = a->database(a->resolve("contact", ""));
  json result = json::object();
  if (!hasTable(db, "chat_room"))
    return result;
  auto rows =
      query(db, "SELECT ext_buffer FROM chat_room WHERE username=" + quote(room) + " LIMIT 1");
  if (rows.empty())
    return result;
  std::set<std::string> members;
  for (auto &r : groupMembers(a, room))
    members.insert(str(r, "username"));
  std::function<void(const std::string &, int)> walk = [&](const auto &bytes, int depth) {
    if (depth > 7)
      return;
    auto fields = proto(bytes);
    std::string username, nick;
    for (auto &f : fields) {
      if (f.wire == 2 && f.field == 1 && members.count(f.bytes))
        username = f.bytes;
      if (f.wire == 2 && f.field == 2)
        nick = f.bytes;
    }
    if (!username.empty() && !nick.empty())
      result[username] = nick;
    for (auto &f : fields)
      if (f.wire == 2)
        walk(f.bytes, depth + 1);
  };
  walk(unhex(str(rows[0], "ext_buffer")), 0);
  return result;
}
API int32_t wcdb_get_group_members(int64_t h, const char *room, void **out) {
  return guarded([&]() { return output(groupMembers(account(h), text(room)), out); });
}
API int32_t wcdb_get_group_nicknames(int64_t h, const char *room, void **out) {
  return guarded([&]() { return output(groupNicknames(account(h), text(room)), out); });
}
API int32_t wcdb_get_group_member_count(int64_t h, const char *room, int32_t *count) {
  return guarded([&]() {
    if (!count)
      return -1;
    *count = static_cast<int32_t>(groupMembers(account(h), text(room)).size());
    return 0;
  });
}
API int32_t wcdb_get_group_member_counts(int64_t h, const char *ids, void **out) {
  return guarded([&]() {
    json r = json::object();
    for (auto &id : json::parse(text(ids)))
      r[id.get<std::string>()] = groupMembers(account(h), id.get<std::string>()).size();
    return output(r, out);
  });
}
API int32_t wcdb_get_chat_room_ext_buffer(int64_t h, const char *room, void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto rows =
        query(a->database(a->resolve("contact", "")),
              "SELECT ext_buffer FROM chat_room WHERE username=" + quote(text(room)) + " LIMIT 1");
    return output(rows.empty() ? json::object() : rows[0], out);
  });
}
API int32_t wcdb_get_contact_status(int64_t h, const char *ids, void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto db = a->database(a->resolve("contact", ""));
    json map = json::object();
    for (auto &id : json::parse(text(ids))) {
      auto username = id.get<std::string>();
      auto rows = query(db, "SELECT * FROM contact WHERE username=" + quote(username) + " LIMIT 1");
      if (rows.empty())
        continue;
      bool muted = false;
      for (auto &f : proto(unhex(str(rows[0], "extra_buffer"))))
        if (f.field == 12 && f.wire == 0)
          muted = f.integer != 0;
      json state = {{"isMuted", muted}};
      state["isFolded"] = (number(rows[0], "flag") & 0x10000000) != 0;
      map[username] = state;
    }
    return output(map, out);
  });
}
API int32_t wcdb_get_head_image_buffers(int64_t h, const char *ids, void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto paths = a->files("head_image");
    json map = json::object();
    if (!paths.empty()) {
      auto db = a->database(paths[0]);
      for (auto &id : json::parse(text(ids))) {
        auto u = id.get<std::string>();
        auto rows = query(db, "SELECT image_buffer FROM head_image WHERE username=" + quote(u) +
                                  " LIMIT 1");
        if (!rows.empty())
          map[u] = rows[0]["image_buffer"];
      }
    }
    return output(map, out);
  });
}

#include "media_sns.inc"
#include "mutations.inc"
#include "statistics.inc"

#include "monitor.inc"
#include "navigation.inc"
