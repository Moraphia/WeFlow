static json privateCounter(const std::string &sid) {
  return {
      {"session_id", sid}, {"incoming_count", 0},    {"outgoing_count", 0},
      {"replied", false},  {"first_incoming_ts", 0}, {"first_reply_ts", 0},
      {"latest_ts", 0},    {"anchor_local_id", 0},   {"anchor_create_time", 0}};
}
static void countPrivate(json &c, const json &r, bool sent) {
  auto t = number(r, "create_time");
  auto direction = sent ? "outgoing_count" : "incoming_count";
  c[direction] = number(c, direction) + 1;
  c["latest_ts"] = t;
  if (!number(c, "anchor_local_id")) {
    c["anchor_local_id"] = r["local_id"];
    c["anchor_create_time"] = t;
  }
  if (!sent && !number(c, "first_incoming_ts"))
    c["first_incoming_ts"] = t;
  if (sent && number(c, "first_incoming_ts") && !number(c, "first_reply_ts")) {
    c["first_reply_ts"] = t;
    c["replied"] = true;
  }
}
API int32_t wcdb_get_my_footprint_stats(int64_t h, const char *options,
                                        void **out) {
  return guarded([&]() {
    auto started = std::chrono::steady_clock::now();
    auto a = account(h);
    auto opts = json::parse(text(options));
    auto me = str(opts, "my_accountId", a->myId);
    auto begin = int(number(opts, "begin")), end = int(number(opts, "end"));
    json sessions = json::array(), segments = json::array(),
         mentions = json::array(), groups = json::array();
    int inbound = 0, replied = 0, outbound = 0, totalMentions = 0;
    bool privateTruncated = false, mentionTruncated = false;
    auto privateLimit = number(opts, "private_limit"),
         mentionLimit = number(opts, "mention_limit");
    for (auto &id : opts.value("private_session_ids", json::array())) {
      auto sid = id.get<std::string>();
      auto c = privateCounter(sid), segment = privateCounter(sid);
      int index = 0;
      auto finish = [&]() {
        if (!number(segment, "latest_ts"))
          return;
        segment["segment_index"] = index++;
        segment["end_ts"] = segment["latest_ts"];
        segment["duration_sec"] =
            number(segment, "end_ts") - number(segment, "start_ts");
        segment["message_count"] = number(segment, "incoming_count") +
                                   number(segment, "outgoing_count");
        if (!privateLimit || int64_t(segments.size()) < privateLimit)
          segments.push_back(segment);
        else
          privateTruncated = true;
      };
      eachMessage(a, json::array({sid}), begin, end, [&](auto &, auto &r) {
        auto t = number(r, "create_time");
        bool sent = number(r, "computed_is_send") == 1 ||
                    str(r, "sender_username") == me;
        if (number(segment, "latest_ts") &&
            t - number(segment, "latest_ts") > 300) {
          finish();
          segment = privateCounter(sid);
        }
        if (!segment.contains("start_ts"))
          segment["start_ts"] = t;
        countPrivate(c, r, sent);
        countPrivate(segment, r, sent);
      });
      finish();
      if (number(c, "latest_ts")) {
        sessions.push_back(c);
        inbound += number(c, "incoming_count") > 0;
        outbound += number(c, "outgoing_count") > 0;
        replied += c.value("replied", false);
      }
    }
    for (auto &id : opts.value("group_session_ids", json::array())) {
      auto sid = id.get<std::string>();
      int count = 0;
      int64_t latest = 0;
      eachMessage(a, json::array({sid}), begin, end, [&](auto &, auto &r) {
        if (number(r, "computed_is_send") == 1 ||
            (number(r, "local_type") & 0xffffffffLL) != 1 || me.empty())
          return;
        auto source = str(r, "source");
        auto decoded = unhex(source);
        if (!decoded.empty())
          source = decoded;
        auto content = str(r, "message_content");
        if (source.find(me) == std::string::npos &&
            content.find("@" + me) == std::string::npos)
          return;
        ++count;
        ++totalMentions;
        latest = number(r, "create_time");
        if (!mentionLimit || int64_t(mentions.size()) < mentionLimit)
          mentions.push_back({{"session_id", sid},
                              {"local_id", r["local_id"]},
                              {"create_time", r["create_time"]},
                              {"sender_username", str(r, "sender_username")},
                              {"message_content", content},
                              {"source", source}});
        else
          mentionTruncated = true;
      });
      if (count)
        groups.push_back(
            {{"session_id", sid}, {"count", count}, {"latest_ts", latest}});
    }
    return output(
        {{"summary",
          {{"private_inbound_people", inbound},
           {"private_outbound_people", outbound},
           {"private_replied_people", replied},
           {"private_reply_rate", inbound ? double(replied) / inbound : 0.0},
           {"mention_count", totalMentions},
           {"mention_group_count", groups.size()}}},
         {"private_sessions", sessions},
         {"private_segments", segments},
         {"mentions", mentions},
         {"mention_groups", groups},
         {"diagnostics",
          {{"truncated", privateTruncated || mentionTruncated},
           {"private_truncated", privateTruncated},
           {"mention_truncated", mentionTruncated},
           {"scanned_dbs", a->files("message").size()},
           {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - started)
                              .count()}}}},
        out);
  });
}
API int32_t wcdb_ai_query_session_candidates(int64_t h, const char *options,
                                             void **out) {
  return guarded([&]() {
    auto a = account(h);
    auto opts = json::parse(text(options));
    auto sid = str(opts, "session_id"), term = str(opts, "keyword");
    auto ids = sid.empty() ? sessionIds(a) : json::array({sid});
    std::map<std::pair<std::string, std::string>, json> map;
    eachMessage(a, ids, int(number(opts, "begin_timestamp")),
                int(number(opts, "end_timestamp")),
                [&](auto &session, auto &r) {
                  auto content = str(r, "compress_content");
                  if (content.empty())
                    content = str(r, "message_content");
                  if (!term.empty() && content.find(term) == std::string::npos)
                    return;
                  auto sender = str(r, "sender_username");
                  auto &c = map[{session, sender}];
                  if (c.is_null())
                    c = {{"session_id", session},
                         {"sender_username", sender},
                         {"match_count", 0},
                         {"first_timestamp", number(r, "create_time")}};
                  c["match_count"] = number(c, "match_count") + 1;
                  c["last_timestamp"] = r["create_time"];
                  c["latest_message"] = r;
                });
    json rows = json::array();
    for (auto &p : map)
      rows.push_back(p.second);
    std::sort(rows.begin(), rows.end(), [](auto &x, auto &y) {
      return number(x, "match_count") > number(y, "match_count");
    });
    json page = json::array();
    size_t first =
               std::min(rows.size(),
                        size_t(std::max<int64_t>(0, number(opts, "offset")))),
           last =
               std::min(rows.size(), first + size_t(number(opts, "limit") > 0
                                                        ? number(opts, "limit")
                                                        : 30));
    for (size_t i = first; i < last; ++i)
      page.push_back(rows[i]);
    return output({{"candidates", page},
                   {"total", rows.size()},
                   {"has_more", last < rows.size()}},
                  out);
  });
}
