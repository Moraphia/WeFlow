static json streakFor(const std::set<std::string> &dates) {
  int longest = 0, current = 0;
  int64_t previous = -2;
  std::string start, bestStart, bestEnd;
  for (auto &date : dates) {
    std::tm tm{};
    std::istringstream input(date);
    input >> std::get_time(&tm, "%Y-%m-%d");
    auto day = _mkgmtime(&tm) / 86400;
    if (day == previous + 1)
      ++current;
    else {
      current = 1;
      start = date;
    }
    if (current > longest) {
      longest = current;
      bestStart = start;
      bestEnd = date;
    }
    previous = day;
  }
  return {{"days", longest}, {"startDate", bestStart}, {"endDate", bestEnd}};
}
static json topPhrases(const std::map<std::string, int64_t> &counts,
                       size_t limit = 100) {
  std::vector<std::pair<std::string, int64_t>> list(counts.begin(),
                                                    counts.end());
  std::sort(list.begin(), list.end(), [](auto &a, auto &b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  json result = json::array();
  for (size_t i = 0; i < std::min(limit, list.size()); ++i)
    result.push_back({{"phrase", list[i].first}, {"count", list[i].second}});
  return result;
}
static json emptyHeatmap() {
  json h = json::array();
  for (int i = 0; i < 7; ++i)
    h.push_back(json::array());
  for (int w = 0; w < 7; ++w)
    for (int hour = 0; hour < 24; ++hour)
      h[w].push_back(0);
  return h;
}
static json aggregate(std::shared_ptr<Account> a, json ids, int begin, int end,
                      bool extras = false, int peakBegin = 0, int peakEnd = 0) {
  if (ids.empty())
    ids = sessionIds(a);
  json result = {{"total", 0},
                 {"words", 0},
                 {"sent", 0},
                 {"received", 0},
                 {"sessions", json::object()},
                 {"monthly", json::object()},
                 {"weekday", json::object()},
                 {"daily", json::object()},
                 {"hourly", std::vector<int64_t>(24, 0)},
                 {"typeCounts", json::object()},
                 {"idMap", json::object()}};
  json heat = emptyHeatmap(), midnight = json::object(),
       conversation = json::object(), response = json::object(),
       peak = json::object();
  std::map<std::string, std::set<std::string>> days;
  std::map<std::string, int64_t> lastTime, responseSum, responseMin,
      responseCount;
  std::map<std::string, bool> lastSent;
  std::map<std::string, int64_t> phrases;
  for (auto &id : ids) {
    auto sid = id.get<std::string>();
    result["sessions"][sid] = {{"sent", 0},
                               {"received", 0},
                               {"total", 0},
                               {"monthly", json::object()},
                               {"senders", json::object()},
                               {"typeCounts", json::object()},
                               {"words", 0}};
    conversation[sid] = {{"initiated", 0}, {"received", 0}};
    midnight[sid] = 0;
    peak[sid] = 0;
  }
  eachMessage(a, ids, begin, end, [&](const auto &sid, const auto &row) {
    auto &s = result["sessions"][sid];
    auto timestamp = number(row, "create_time");
    auto tm = local(timestamp);
    auto date = dateKey(timestamp);
    auto month = std::to_string(tm.tm_mon + 1);
    auto type = std::to_string(
        static_cast<uint64_t>(number(row, "local_type")) & 0xffffffffULL);
    auto sender = str(row, "sender_username");
    bool sent = number(row, "computed_is_send") == 1 ||
                (!a->myId.empty() && sender == a->myId);
    auto side = sent ? "sent" : "received";
    result["monthly"][month] = number(result["monthly"], month) + 1;
    auto weekday = std::to_string(tm.tm_wday);
    result["weekday"][weekday] = number(result["weekday"], weekday) + 1;
    if (!result.contains("firstTime") ||
        timestamp < number(result, "firstTime"))
      result["firstTime"] = timestamp;
    if (timestamp > number(result, "lastTime"))
      result["lastTime"] = timestamp;
    s["lastTime"] = timestamp;
    result["total"] = number(result, "total") + 1;
    result[side] = number(result, side) + 1;
    s["total"] = number(s, "total") + 1;
    s[side] = number(s, side) + 1;
    s["monthly"][month] = number(s["monthly"], month) + 1;
    result["daily"][date] = number(result["daily"], date) + 1;
    result["hourly"][tm.tm_hour] =
        result["hourly"][tm.tm_hour].get<int64_t>() + 1;
    result["typeCounts"][type] = number(result["typeCounts"], type) + 1;
    s["typeCounts"][type] = number(s["typeCounts"], type) + 1;
    if (!sender.empty()) {
      s["senders"][sender] = number(s["senders"], sender) + 1;
      result["idMap"][sender] = sender;
    }
    if (type == "1") {
      auto content = str(row, "compress_content");
      if (content.empty())
        content = str(row, "message_content");
      auto count = charCount(content);
      s["words"] = number(s, "words") + count;
      result["words"] = number(result, "words") + count;
      if (extras && count >= 2 && count <= 100)
        ++phrases[content];
    }
    if (!s.contains("first_time") || timestamp < number(s, "first_time"))
      s["first_time"] = timestamp;
    if (timestamp > number(s, "last_time"))
      s["last_time"] = timestamp;
    if (extras) {
      days[sid].insert(date);
      heat[tm.tm_wday][tm.tm_hour] =
          heat[tm.tm_wday][tm.tm_hour].get<int64_t>() + 1;
      if (tm.tm_hour < 5)
        midnight[sid] = number(midnight, sid) + 1;
      if (peakBegin > 0 && timestamp >= peakBegin && timestamp <= peakEnd)
        peak[sid] = number(peak, sid) + 1;
      auto gap = timestamp - lastTime[sid];
      if (lastTime[sid] == 0 || gap > 3600) {
        auto direction = sent ? "initiated" : "received";
        conversation[sid][direction] = number(conversation[sid], direction) + 1;
      }
      if (lastTime[sid] > 0 && lastSent[sid] && !sent && gap > 0 &&
          gap <= 3600) {
        responseSum[sid] += gap;
        ++responseCount[sid];
        if (!responseMin[sid] || gap < responseMin[sid])
          responseMin[sid] = gap;
      }
      lastTime[sid] = timestamp;
      lastSent[sid] = sent;
    }
  });
  if (extras) {
    json best = {
        {"days", 0}, {"sessionId", ""}, {"startDate", ""}, {"endDate", ""}};
    for (auto &id : ids) {
      auto sid = id.get<std::string>();
      auto streak = streakFor(days[sid]);
      if (number(streak, "days") > number(best, "days")) {
        best = streak;
        best["sessionId"] = sid;
      }
      response[sid] = {{"avg", responseCount[sid] ? double(responseSum[sid]) /
                                                        responseCount[sid]
                                                  : 0.0},
                       {"fastest", responseMin[sid]},
                       {"count", responseCount[sid]}};
    }
    result["heatmap"] = heat;
    result["midnight"] = midnight;
    result["conversation"] = conversation;
    result["response"] = response;
    result["peakDay"] = peak;
    result["topPhrases"] = topPhrases(phrases);
    result["streak"] = best;
  }
  return result;
}
API int32_t wcdb_get_aggregate_stats(int64_t h, const char *ids, int32_t begin,
                                     int32_t end, void **out) {
  return guarded([&]() {
    return output(aggregate(account(h), json::parse(text(ids)), begin, end),
                  out);
  });
}
API int32_t wcdb_get_group_stats(int64_t h, const char *sid, int32_t begin,
                                 int32_t end, void **out) {
  return guarded([&]() {
    return output(aggregate(account(h), json::array({text(sid)}), begin, end),
                  out);
  });
}
API int32_t wcdb_get_annual_report_stats(int64_t h, const char *ids,
                                         int32_t begin, int32_t end,
                                         void **out) {
  return wcdb_get_aggregate_stats(h, ids, begin, end, out);
}
API int32_t wcdb_get_annual_report_extras(int64_t h, const char *ids,
                                          int32_t begin, int32_t end,
                                          int32_t peakBegin, int32_t peakEnd,
                                          void **out) {
  return guarded([&]() {
    return output(aggregate(account(h), json::parse(text(ids)), begin, end,
                            true, peakBegin, peakEnd),
                  out);
  });
}
API int32_t wcdb_get_available_years(int64_t h, const char *ids, void **out) {
  return guarded([&]() {
    std::set<int> years;
    eachMessage(account(h), json::parse(text(ids)), 0, 0, [&](auto &, auto &r) {
      years.insert(local(number(r, "create_time")).tm_year + 1900);
    });
    return output(years, out);
  });
}
static json typeStats(std::shared_ptr<Account> a, const std::string &sid,
                      int begin, int end, bool dates = false) {
  json stats = {{"total_messages", 0},    {"text_messages", 0},
                {"image_messages", 0},    {"video_messages", 0},
                {"voice_messages", 0},    {"emoji_messages", 0},
                {"file_messages", 0},     {"call_messages", 0},
                {"transfer_messages", 0}, {"red_packet_messages", 0},
                {"group_my_messages", 0}, {"group_sender_count", 0},
                {"first_timestamp", 0},   {"last_timestamp", 0}};
  std::set<std::string> senders;
  json dateCounts = json::object();
  eachMessage(a, json::array({sid}), begin, end, [&](auto &, auto &r) {
    auto raw = number(r, "local_type"), type = raw & 0xffffffffLL,
         sub = raw >> 32;
    auto content = str(r, "compress_content");
    if (content.empty())
      content = str(r, "message_content");
    stats["total_messages"] = number(stats, "total_messages") + 1;
    std::string category = type == 1                  ? "text"
                           : type == 3                ? "image"
                           : type == 34               ? "voice"
                           : type == 43 || type == 62 ? "video"
                           : type == 47               ? "emoji"
                           : type == 50               ? "call"
                                                      : "";
    if (type == 49) {
      std::smatch m;
      if (!sub && std::regex_search(content, m,
                                    std::regex("<type>\\s*([0-9]+)\\s*</type>",
                                               std::regex::icase)))
        sub = std::stoll(m[1].str());
      category = sub == 6 || sub == 40 ? "file"
                 : sub == 2000         ? "transfer"
                 : sub == 2001         ? "red_packet"
                                       : "";
    }
    if (!category.empty())
      stats[category + "_messages"] = number(stats, category + "_messages") + 1;
    auto t = number(r, "create_time");
    if (!number(stats, "first_timestamp") ||
        t < number(stats, "first_timestamp"))
      stats["first_timestamp"] = t;
    if (t > number(stats, "last_timestamp"))
      stats["last_timestamp"] = t;
    if (number(r, "computed_is_send") == 1)
      stats["group_my_messages"] = number(stats, "group_my_messages") + 1;
    auto sender = str(r, "sender_username");
    if (!sender.empty())
      senders.insert(sender);
    if (dates) {
      auto day = dateKey(t);
      dateCounts[day] = number(dateCounts, day) + 1;
    }
  });
  stats["group_sender_count"] = senders.size();
  if (dates)
    stats["date_counts"] = dateCounts;
  return stats;
}
API int32_t wcdb_get_session_message_type_stats(int64_t h, const char *sid,
                                                int32_t begin, int32_t end,
                                                void **out) {
  return guarded([&]() {
    return output(typeStats(account(h), text(sid), begin, end), out);
  });
}
API int32_t wcdb_get_session_message_type_stats_batch(int64_t h,
                                                      const char *ids,
                                                      const char *options,
                                                      void **out) {
  return guarded([&]() {
    auto opts = json::parse(text(options).empty() ? "{}" : text(options));
    json result = json::object();
    for (auto &id : json::parse(text(ids)))
      result[id.get<std::string>()] = typeStats(
          account(h), id.get<std::string>(), int(number(opts, "begin")),
          int(number(opts, "end")), opts.value("include_date_counts", false));
    return output(result, out);
  });
}
API int32_t wcdb_get_dual_report_stats(int64_t h, const char *sid,
                                       int32_t begin, int32_t end, void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto session = text(sid);
    auto full = aggregate(a, json::array({session}), begin, end, true);
    auto s = full["sessions"][session];
    json counts = {{"total", full["total"]},
                   {"words", full["words"]},
                   {"image", number(full["typeCounts"], "3")},
                   {"voice", number(full["typeCounts"], "34")},
                   {"emoji", number(full["typeCounts"], "47")}};
    std::map<std::string, int64_t> mine, theirs, phrases;
    std::map<std::pair<bool, std::string>, int64_t> emojis;
    std::map<std::string, std::string> urls;
    std::set<std::string> days;
    eachMessage(a, json::array({session}), begin, end, [&](auto &, auto &row) {
      bool sent = number(row, "computed_is_send") == 1;
      auto content = str(row, "compress_content");
      if (content.empty())
        content = str(row, "message_content");
      auto type = number(row, "local_type") & 0xffffffffLL;
      if (type == 1 && charCount(content) >= 2 && charCount(content) <= 100) {
        ++phrases[content];
        ++(sent ? mine : theirs)[content];
      }
      if (type == 47) {
        std::smatch match;
        if (std::regex_search(content, match,
                              std::regex("md5=['\"]([0-9a-fA-F]{32})"))) {
          auto md5 = match[1].str();
          ++emojis[{sent, md5}];
          if (std::regex_search(content, match,
                                std::regex("cdnurl=['\"]([^'\"]+)")))
            urls[md5] = match[1].str();
        }
      }
      days.insert(dateKey(number(row, "create_time")));
    });
    json emojiRows = json::array();
    for (auto &e : emojis)
      emojiRows.push_back({{"isMe", e.first.first},
                           {"is_me", e.first.first},
                           {"md5", e.first.second},
                           {"count", e.second},
                           {"url", urls[e.first.second]}});
    return output({{"counts", counts},
                   {"emojis", emojiRows},
                   {"phrases", topPhrases(phrases)},
                   {"myPhrases", topPhrases(mine)},
                   {"friendPhrases", topPhrases(theirs)},
                   {"heatmap", full["heatmap"]},
                   {"initiative", full["conversation"][session]},
                   {"response", full["response"][session]},
                   {"monthly", s["monthly"]},
                   {"streak", streakFor(days)}},
                  out);
  });
}
