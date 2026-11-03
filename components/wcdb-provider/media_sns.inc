static fs::path optionalDb(std::shared_ptr<Account> a, const std::string &kind,
                           const std::string &hint = "") {
  if (!hint.empty() && fs::is_regular_file(fs::u8path(hint)))
    return a->resolve(kind, hint);
  auto paths = a->files(kind);
  return paths.empty() ? fs::path() : paths.front();
}
static json hardlink(std::shared_ptr<Account> a, const std::string &digest,
                     const std::string &type, const std::string &hint = "") {
  if (!std::regex_match(digest, std::regex("[0-9a-fA-F]{32}")))
    throw std::runtime_error("Expected a media MD5");
  auto path = optionalDb(a, "hardlink", hint);
  if (path.empty())
    return json::object();
  auto db = a->database(path);
  auto table = type + "_hardlink_info_v4";
  if (!hasTable(db, table))
    return json::object();
  auto rows = query(db, "SELECT * FROM " + identifier(table) +
                            " WHERE lower(md5)=lower(" + quote(digest) +
                            ") ORDER BY modify_time DESC LIMIT 1");
  if (rows.empty())
    return json::object();
  auto result = rows[0];
  auto root = a->storage.parent_path();
  auto filename = str(result, "file_name");
  std::string dir1, dir2;
  if (hasTable(db, "dir2id")) {
    for (auto pair : std::vector<std::pair<std::string, std::string *>>{
             {"dir1", &dir1}, {"dir2", &dir2}}) {
      auto dirs = query(db, "SELECT username FROM dir2id WHERE rowid=" +
                                std::to_string(number(result, pair.first)));
      if (!dirs.empty())
        *pair.second = str(dirs[0], "username");
    }
  }
  std::vector<fs::path> candidates;
  auto folder = type == "image" ? "Img" : type == "video" ? "Video" : "File";
  if (!dir1.empty() && !dir2.empty()) {
    candidates.push_back(root / "msg/attach" / fs::u8path(dir1) /
                         fs::u8path(dir2) / folder / fs::u8path(filename));
    candidates.push_back(root / "msg/attach" / fs::u8path(dir1) /
                         fs::u8path(dir2) / fs::u8path(filename));
  }
  candidates.push_back(root / "msg" /
                       (type == "video"   ? "video"
                        : type == "image" ? "image"
                                          : "file") /
                       fs::u8path(dir2) / fs::u8path(filename));
  if (type == "image" && fs::u8path(filename).extension().empty()) {
    auto original = candidates;
    for (auto p : original) {
      p += ".dat";
      candidates.push_back(p);
    }
  }
  auto selected = candidates.front();
  for (auto &candidate : candidates)
    if (fs::is_regular_file(candidate)) {
      selected = candidate;
      break;
    }
  result["full_path"] = selected.u8string();
  result["file_name"] = selected.filename().u8string();
  result["exists"] = fs::is_regular_file(selected);
  result["dir1_name"] = dir1;
  result["dir2_name"] = dir2;
  return result;
}
API int32_t wcdb_resolve_image_hardlink(int64_t h, const char *digest,
                                        const char *accountDir, void **out) {
  return guarded([&]() {
    auto a = account(h);
    if (accountDir && *accountDir &&
        fs::weakly_canonical(fs::u8path(accountDir)) !=
            fs::weakly_canonical(a->storage.parent_path()))
      return -1;
    return output(hardlink(a, text(digest), "image"), out);
  });
}
API int32_t wcdb_resolve_video_hardlink_md5(int64_t h, const char *digest,
                                            const char *hint, void **out) {
  return guarded([&]() {
    return output(hardlink(account(h), text(digest), "video", text(hint)), out);
  });
}
API int32_t wcdb_resolve_image_hardlink_batch(int64_t h, const char *requests,
                                              void **out) {
  return guarded([&]() {
    json rows = json::array();
    int index = 0;
    for (auto &req : json::parse(text(requests))) {
      auto digest = str(req, "md5");
      auto data = hardlink(account(h), digest, "image");
      rows.push_back({{"index", index++},
                      {"md5", digest},
                      {"success", !data.empty()},
                      {"data", data}});
    }
    return output(rows, out);
  });
}
API int32_t wcdb_resolve_video_hardlink_md5_batch(int64_t h,
                                                  const char *requests,
                                                  void **out) {
  return guarded([&]() {
    json rows = json::array();
    int index = 0;
    for (auto &req : json::parse(text(requests))) {
      auto digest = str(req, "md5");
      auto data = hardlink(account(h), digest, "video", str(req, "db_path"));
      rows.push_back({{"index", index++},
                      {"md5", digest},
                      {"success", !data.empty()},
                      {"data", data}});
    }
    return output(rows, out);
  });
}
API int32_t wcdb_list_media_dbs(int64_t h, void **out) {
  return guarded([&]() {
    json rows = json::array();
    for (auto &p : account(h)->files("media"))
      rows.push_back(p.u8string());
    return output(rows, out);
  });
}
API int32_t wcdb_get_media_schema_summary(int64_t h, const char *hint,
                                          void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto db = a->database(a->resolve("media", text(hint)));
    json result = json::object();
    for (auto &t :
         query(db, "SELECT name FROM sqlite_master WHERE type='table'"))
      result[str(t, "name")] = tableColumns(db, str(t, "name"));
    return output(result, out);
  });
}
static std::string voiceData(std::shared_ptr<Account> a, const std::string &sid,
                             int created, int localId, int64_t serverId,
                             const json &candidates) {
  for (auto &file : a->files("media")) {
    auto db = a->database(file);
    if (!hasTable(db, "VoiceInfo"))
      continue;
    auto columns = columnNames(db, "VoiceInfo");
    std::vector<std::string> conditions;
    if (serverId > 0 && columns.count("svr_id"))
      conditions.push_back("svr_id=" + std::to_string(serverId));
    std::string chatCondition;
    if (hasTable(db, "Name2Id")) {
      std::set<std::string> names{sid};
      for (auto &c : candidates)
        if (c.is_string())
          names.insert(c.get<std::string>());
      std::string list;
      for (auto &n : names) {
        if (!list.empty())
          list += ",";
        list += quote(n);
      }
      chatCondition =
          "chat_name_id IN (SELECT rowid FROM Name2Id WHERE user_name IN (" +
          list + ")) AND ";
    }
    if (localId > 0)
      conditions.push_back(
          chatCondition + "local_id=" + std::to_string(localId) +
          (created > 0
               ? " AND abs(create_time-" + std::to_string(created) + ")<=5"
               : ""));
    if (created > 0)
      conditions.push_back(chatCondition + "abs(create_time-" +
                           std::to_string(created) + ")<=5");
    for (auto &condition : conditions) {
      auto rows =
          query(db, "SELECT * FROM VoiceInfo WHERE " + condition +
                        " ORDER BY create_time," +
                        (columns.count("data_index")
                             ? std::string("CAST(data_index AS INTEGER)")
                             : std::string("local_id")));
      if (rows.empty())
        continue;
      auto first = rows[0];
      std::string data;
      for (auto &r : rows)
        if (number(r, "local_id") == number(first, "local_id") &&
            number(r, "create_time") == number(first, "create_time"))
          data += str(r, "voice_data");
      if (!data.empty())
        return data;
    }
  }
  return {};
}
API int32_t wcdb_get_voice_data(int64_t h, const char *sid, int32_t created,
                                int32_t localId, int64_t serverId,
                                const char *candidates, void **out) {
  return guarded([&]() {
    return rawOutput(
        voiceData(
            account(h), text(sid), created, localId, serverId,
            json::parse(text(candidates).empty() ? "[]" : text(candidates))),
        out);
  });
}
API int32_t wcdb_get_voice_data_batch(int64_t h, const char *requests,
                                      void **out) {
  return guarded([&]() {
    json rows = json::array();
    int index = 0;
    for (auto &req : json::parse(text(requests)))
      rows.push_back(
          {{"index", index++},
           {"hex",
            voiceData(account(h), str(req, "session_id"),
                      int(number(req, "create_time")),
                      int(number(req, "local_id")), number(req, "svr_id"),
                      req.value("candidates", json::array()))}});
    return output(rows, out);
  });
}
static std::string emoticonValue(std::shared_ptr<Account> a,
                                 const std::string &digest, bool caption,
                                 const std::string &hint = "") {
  auto path = optionalDb(a, "emoticon", hint);
  if (path.empty())
    return {};
  auto db = a->database(path);
  if (hasTable(db, "kNonStoreEmoticonTable")) {
    auto rows = query(
        db, "SELECT * FROM kNonStoreEmoticonTable WHERE lower(md5)=lower(" +
                quote(digest) + ") LIMIT 1");
    if (!rows.empty()) {
      if (caption) {
        auto value = str(rows[0], "caption");
        if (!value.empty())
          return value;
      } else
        for (auto &key : {"cdn_url", "extern_url", "tp_url", "thumb_url"}) {
          auto value = str(rows[0], key);
          if (!value.empty())
            return value;
        }
    }
  }
  if (caption && hasTable(db, "kStoreEmoticonCaptionsTable")) {
    auto rows = query(db, "SELECT caption_ FROM kStoreEmoticonCaptionsTable "
                          "WHERE lower(md5_)=lower(" +
                              quote(digest) +
                              ") ORDER BY CASE WHEN language_ IN "
                              "('zh_CN','zh') THEN 0 ELSE 1 END LIMIT 1");
    if (!rows.empty())
      return str(rows[0], "caption_");
  }
  return {};
}
API int32_t wcdb_get_emoticon_cdn_url(int64_t h, const char *hint,
                                      const char *digest, void **out) {
  return guarded([&]() {
    return rawOutput(emoticonValue(account(h), text(digest), false, text(hint)),
                     out);
  });
}
API int32_t wcdb_get_emoticon_caption(int64_t h, const char *hint,
                                      const char *digest, void **out) {
  return guarded([&]() {
    return rawOutput(emoticonValue(account(h), text(digest), true, text(hint)),
                     out);
  });
}
API int32_t wcdb_get_emoticon_caption_strict(int64_t h, const char *digest,
                                             void **out) {
  return guarded([&]() {
    return rawOutput(emoticonValue(account(h), text(digest), true), out);
  });
}
API int32_t wcdb_scan_media_stream(int64_t h, const char *ids, int32_t type,
                                   int32_t begin, int32_t end, int32_t limit,
                                   int32_t offset, void **out,
                                   int32_t *hasMore) {
  return guarded([&]() {
    if (!hasMore)
      return -1;
    auto a = account(h);
    auto sessions = json::parse(text(ids));
    if (sessions.empty())
      sessions = sessionIds(a);
    json rows = json::array();
    eachMessage(a, sessions, begin, end, [&](auto &sid, auto &r) {
      if (!type || (number(r, "local_type") & 0xffffffffLL) == type) {
        auto item = r;
        item["session_id"] = sid;
        rows.push_back(item);
      }
    });
    std::sort(rows.begin(), rows.end(), [](auto &a, auto &b) {
      return number(a, "create_time") > number(b, "create_time");
    });
    size_t first = std::min(rows.size(), size_t(std::max(0, offset))),
           last =
               std::min(rows.size(), first + size_t(limit > 0 ? limit : 500));
    json page = json::array();
    for (size_t i = first; i < last; ++i)
      page.push_back(rows[i]);
    *hasMore = last < rows.size() ? 1 : 0;
    return output(page, out);
  });
}
static uint64_t unsignedId(const json &value) {
  return value.is_string() ? std::stoull(value.get<std::string>())
                           : static_cast<uint64_t>(value.get<int64_t>());
}
static json snsTimeline(std::shared_ptr<Account> a, int limit, int offset,
                        const std::string &users, const std::string &keyword,
                        int begin, int end) {
  auto path = optionalDb(a, "sns");
  if (path.empty())
    return json::array();
  auto db = a->database(path);
  if (!hasTable(db, "SnsTimeLine"))
    return json::array();
  std::string sql = "SELECT * FROM SnsTimeLine WHERE 1=1";
  if (!users.empty()) {
    auto ids = users.front() == '[' ? json::parse(users) : json::array({users});
    std::string list;
    for (auto &id : ids) {
      if (!list.empty())
        list += ",";
      list += quote(id.get<std::string>());
    }
    if (!list.empty())
      sql += " AND user_name IN (" + list + ")";
  }
  if (!keyword.empty())
    sql += " AND instr(content," + quote(keyword) + ")>0";
  if (begin > 0)
    sql +=
        " AND tid>=" +
        std::to_string(static_cast<int64_t>(uint64_t(begin) * 1000ULL << 23));
  if (end > 0)
    sql += " AND tid<=" + std::to_string(static_cast<int64_t>(
                              (uint64_t(end) * 1000ULL + 999) << 23));
  sql += " ORDER BY tid DESC LIMIT " +
         std::to_string(limit > 0 ? limit : 20000) + " OFFSET " +
         std::to_string(std::max(0, offset));
  auto rows = query(db, sql);
  for (auto &r : rows) {
    auto id = unsignedId(r["tid"]);
    r["tid"] = std::to_string(id);
    r["create_time"] = (id >> 23) / 1000;
    r["username"] = str(r, "user_name");
  }
  return rows;
}
API int32_t wcdb_get_sns_timeline(int64_t h, int32_t limit, int32_t offset,
                                  const char *users, const char *keyword,
                                  int32_t begin, int32_t end, void **out) {
  return guarded([&]() {
    return output(snsTimeline(account(h), limit, offset, text(users),
                              text(keyword), begin, end),
                  out);
  });
}
API int32_t wcdb_get_sns_usernames(int64_t h, void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto path = optionalDb(a, "sns");
    json ids = json::array();
    if (!path.empty())
      for (auto &r : query(a->database(path),
                           "SELECT DISTINCT user_name FROM SnsTimeLine WHERE "
                           "user_name<>'' ORDER BY user_name"))
        ids.push_back(r["user_name"]);
    return output(ids, out);
  });
}
API int32_t wcdb_get_sns_export_stats(int64_t h, const char *myId, void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto path = optionalDb(a, "sns");
    json stats = {
        {"total_posts", 0}, {"total_friends", 0}, {"my_posts", nullptr}};
    if (!path.empty()) {
      auto db = a->database(path);
      auto r = query(db, "SELECT COUNT(*) AS total_posts,COUNT(DISTINCT "
                         "user_name) AS total_friends FROM SnsTimeLine")[0];
      stats["total_posts"] = r["total_posts"];
      stats["total_friends"] = r["total_friends"];
      auto id = text(myId).empty() ? a->myId : text(myId);
      if (!id.empty())
        stats["my_posts"] = query(
            db, "SELECT COUNT(*) AS count FROM SnsTimeLine WHERE user_name=" +
                    quote(id))[0]["count"];
    }
    return output(stats, out);
  });
}
#include "sns_stats.inc"
