static std::string xmlTag(const std::string &xml, const std::string &tag) {
  std::smatch match;
  if (!std::regex_search(
          xml, match,
          std::regex("<" + tag + "(?:\\s[^>]*)?>([\\s\\S]*?)</" + tag + ">",
                     std::regex::icase)))
    return {};
  auto result = match[1].str();
  if (result.rfind("<![CDATA[", 0) == 0 && result.size() >= 12)
    result = result.substr(9, result.size() - 12);
  return result;
}
API int32_t wcdb_get_sns_annual_stats(int64_t h, int32_t begin, int32_t end,
                                      void **out) {
  return guarded([&]() {
    auto a = account(h);
    json types = {
        {"text", 0}, {"image", 0}, {"video", 0}, {"link", 0}, {"other", 0}};
    std::map<std::string, int64_t> likers, liked;
    int64_t total = 0;
    // Page to bound SQLite result memory, preserving signed 64-bit IDs as
    // strings.
    for (int offset = 0;; offset += 500) {
      auto posts = snsTimeline(a, 500, offset, "", "", begin, end);
      for (auto &post : posts) {
        ++total;
        auto xml = str(post, "content") + str(post, "pack_info_buf");
        auto style = xmlTag(xml, "contentStyle");
        auto type = style == "1"                    ? "image"
                    : style == "15"                 ? "video"
                    : style == "3"                  ? "link"
                    : style == "2" || style.empty() ? "text"
                                                    : "other";
        types[type] = number(types, type) + 1;
        auto users = xmlTag(xml, "LikeUserList");
        if (users.empty())
          users = xmlTag(xml, "like_user_list");
        if (users.empty())
          users = xmlTag(xml, "likeList");
        auto pattern =
            std::regex("<username>([\\s\\S]*?)</username>", std::regex::icase);
        for (auto it =
                 std::sregex_iterator(users.begin(), users.end(), pattern);
             it != std::sregex_iterator(); ++it) {
          auto user = xmlTag(it->str(), "username");
          if (user.empty())
            continue;
          if (str(post, "user_name") == a->myId)
            ++likers[user];
          if (user == a->myId)
            ++liked[str(post, "user_name")];
        }
      }
      if (posts.size() < 500)
        break;
    }
    auto ranked = [](const std::map<std::string, int64_t> &counts) {
      std::vector<std::pair<std::string, int64_t>> rows(counts.begin(),
                                                        counts.end());
      std::sort(rows.begin(), rows.end(),
                [](auto &a, auto &b) { return a.second > b.second; });
      json result = json::array();
      for (size_t i = 0; i < std::min(rows.size(), size_t(20)); ++i)
        result.push_back(
            {{"username", rows[i].first}, {"count", rows[i].second}});
      return result;
    };
    return output({{"totalPosts", total},
                   {"typeCounts", types},
                   {"topLikers", ranked(likers)},
                   {"topLiked", ranked(liked)}},
                  out);
  });
}
