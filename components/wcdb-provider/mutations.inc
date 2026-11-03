struct Writer {
  sqlite3 *db = nullptr;
  bool committed = false;
  Writer(std::shared_ptr<Account> a, const fs::path &path,
         bool createMissing = false) {
    db = openReadOnly(path, a->key, true, createMissing);
    char *error = nullptr;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &error) !=
        SQLITE_OK) {
      std::string message =
          error ? error : "Unable to acquire database write lock";
      sqlite3_free(error);
      sqlite3_close(db);
      db = nullptr;
      throw std::runtime_error(message);
    }
  }
  ~Writer() {
    if (db) {
      if (!committed)
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
      sqlite3_close(db);
    }
  }
  void exec(const std::string &sql) {
    char *error = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
      std::string message = error ? error : "Database write failed";
      sqlite3_free(error);
      throw std::runtime_error(message);
    }
  }
  void commit() {
    exec("COMMIT");
    committed = true;
  }
};
static void bindCell(sqlite3_stmt *statement, int index, const json &cell) {
  if (cell.is_null())
    sqlite3_bind_null(statement, index);
  else if (cell.is_object() && cell.contains("$blob")) {
    auto bytes = unhex(str(cell, "$blob"));
    sqlite3_bind_blob(statement, index, bytes.data(), int(bytes.size()),
                      SQLITE_TRANSIENT);
  } else if (cell.is_object() && cell.contains("$int64"))
    sqlite3_bind_int64(statement, index, std::stoll(str(cell, "$int64")));
  else if (cell.is_number_integer())
    sqlite3_bind_int64(statement, index, cell.get<int64_t>());
  else if (cell.is_number_float())
    sqlite3_bind_double(statement, index, cell.get<double>());
  else {
    auto value = cell.is_string() ? cell.get<std::string>() : cell.dump();
    sqlite3_bind_text(statement, index, value.data(), int(value.size()),
                      SQLITE_TRANSIENT);
  }
}
static int writeBound(sqlite3 *db, const std::string &sql, const json &cells) {
  sqlite3_stmt *raw = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(db));
  std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(
      raw, sqlite3_finalize);
  int index = 1;
  for (auto &cell : cells)
    bindCell(raw, index++, cell);
  if (sqlite3_step(raw) != SQLITE_DONE)
    throw std::runtime_error(sqlite3_errmsg(db));
  return sqlite3_changes(db);
}
static int writeResult(void **out, const std::function<json()> &fn) {
  return guarded([&]() {
    try {
      return output(fn(), out);
    } catch (const std::exception &error) {
      output({{"success", false}, {"error", error.what()}}, out);
      throw;
    }
  });
}
// outError is nullable on success; several ABI callers only free errors.
static int writeErrorResult(void **out, const std::function<json()> &fn) {
  if (out)
    *out = nullptr;
  return guarded([&]() {
    try {
      fn();
      return 0;
    } catch (const std::exception &error) {
      rawOutput(error.what(), out);
      throw;
    }
  });
}
static std::string messageTrigger(const std::string &sid,
                                  const std::string &suffix = "update") {
  return "weflow_keep_" + md5(sid) + "_" + suffix;
}
static void installMessageTriggers(Writer &w, const std::string &sid) {
  auto table = identifier("Msg_" + md5(sid));
  w.exec(
      "CREATE TRIGGER IF NOT EXISTS " + identifier(messageTrigger(sid)) +
      " BEFORE UPDATE ON " + table +
      " WHEN NEW.local_type IN (10000,266287972401) AND "
      "(instr(NEW.message_content,'revokemsg')>0 OR "
      "instr(NEW.message_content,'撤回')>0) BEGIN SELECT RAISE(IGNORE); END");
  w.exec("CREATE TRIGGER IF NOT EXISTS " +
         identifier(messageTrigger(sid, "delete")) + " BEFORE DELETE ON " +
         table + " BEGIN SELECT RAISE(IGNORE); END");
}
API int32_t wcdb_mark_all_sessions_read(int64_t h, void **out) {
  return writeErrorResult(out, [&]() {
    auto a = account(h);
    Writer w(a, a->session);
    auto columns = columnNames(w.db, "SessionTable");
    std::string sql = "UPDATE SessionTable SET unread_count=0";
    for (auto field :
         {"unread_first_msg_srv_id", "unread_first_pat_msg_local_id",
          "unread_first_pat_msg_sort_seq"})
      if (columns.count(field))
        sql += "," + identifier(field) + "=0";
    w.exec(sql);
    auto changed = sqlite3_changes(w.db);
    for (auto table : {"SessionUnreadListTable_1", "SessionUnreadStatTable_1"})
      if (hasTable(w.db, table))
        w.exec("DELETE FROM " + identifier(table));
    w.commit();
    return json{{"success", true}, {"updated", changed}};
  });
}
API int32_t wcdb_reorder_sessions_by_time(int64_t h, void **out) {
  return writeResult(out, [&]() {
    auto a = account(h);
    Writer w(a, a->session);
    auto count = number(
        query(w.db, "SELECT COUNT(*) AS count FROM SessionTable")[0], "count");
    w.exec("UPDATE SessionTable SET sort_timestamp=last_timestamp WHERE "
           "sort_timestamp IS NOT last_timestamp");
    auto updated = sqlite3_changes(w.db);
    w.commit();
    return json{{"success", true},
                {"table", "SessionTable"},
                {"sort_column", "sort_timestamp"},
                {"source_column", "last_timestamp"},
                {"row_count", count},
                {"updated_rows", updated},
                {"already_normalized", updated == 0}};
  });
}
API int32_t wcdb_update_message(int64_t h, const char *sid, int64_t localId,
                                int32_t created, const char *content,
                                void **out) {
  return writeErrorResult(out, [&]() {
    auto a = account(h);
    auto table = "Msg_" + md5(text(sid));
    int updated = 0;
    for (auto &p : a->files("message")) {
      auto db = a->database(p);
      if (!hasTable(db, table))
        continue;
      auto where =
          "local_id=" + std::to_string(localId) +
          (created > 0 ? " AND create_time=" + std::to_string(created) : "");
      if (query(db, "SELECT 1 FROM " + identifier(table) + " WHERE " + where +
                        " LIMIT 1")
              .empty())
        continue;
      Writer w(a, p);
      auto columns = columnNames(w.db, table);
      auto sql = "UPDATE " + identifier(table) + " SET message_content=?";
      if (columns.count("compress_content"))
        sql += ",compress_content=''";
      for (auto field : {"WCDB_CT_message_content", "WCDB_CT_compress_content"})
        if (columns.count(field))
          sql += "," + identifier(field) + "=NULL";
      updated += writeBound(w.db, sql + " WHERE " + where,
                            json::array({text(content)}));
      w.commit();
    }
    if (!updated)
      throw std::runtime_error("No matching message was updated");
    return json{{"success", true}, {"updated", updated}};
  });
}
API int32_t wcdb_delete_message(int64_t h, const char *sid, int64_t localId,
                                int32_t created, const char *hint, void **out) {
  return writeErrorResult(out, [&]() {
    auto a = account(h);
    auto session = text(sid), table = "Msg_" + md5(session);
    auto paths = text(hint).empty()
                     ? a->files("message")
                     : std::vector<fs::path>{a->resolve("message", text(hint))};
    int deleted = 0;
    for (auto &p : paths) {
      auto db = a->database(p);
      if (!hasTable(db, table))
        continue;
      Writer w(a, p);
      bool protectedTable =
          !query(
               w.db,
               "SELECT name FROM sqlite_master WHERE type='trigger' AND name=" +
                   quote(messageTrigger(session, "delete")))
               .empty();
      if (protectedTable)
        w.exec("DROP TRIGGER " + identifier(messageTrigger(session, "delete")));
      deleted +=
          writeBound(w.db,
                     "DELETE FROM " + identifier(table) + " WHERE local_id=?" +
                         (created > 0 ? " AND create_time=?" : ""),
                     created > 0 ? json::array({localId, created})
                                 : json::array({localId}));
      if (protectedTable)
        installMessageTriggers(w, session);
      w.commit();
    }
    if (!deleted)
      throw std::runtime_error("No matching message was deleted");
    return json{{"success", true}, {"deleted", deleted}};
  });
}
API int32_t wcdb_insert_text_message(int64_t h, const char *sid,
                                     const char *content, int64_t status,
                                     int64_t created, void **out) {
  return writeResult(out, [&]() {
    auto a = account(h);
    auto session = text(sid), table = "Msg_" + md5(session);
    auto tables = messageTables(a, session);
    if (tables.empty())
      throw std::runtime_error(
          "The selected conversation has no message table");
    int64_t nextId = 1, sort = 1;
    fs::path target;
    for (auto &t : tables) {
      auto p = fs::u8path(str(t, "db_path"));
      auto rows = query(a->database(p),
                        "SELECT MAX(local_id) id,MAX(sort_seq) seq FROM " +
                            identifier(table));
      nextId = std::max(nextId, number(rows[0], "id") + 1);
      sort = std::max(sort, number(rows[0], "seq") + 1);
      target = p;
    }
    Writer w(a, target);
    int64_t sender = 0;
    auto names = query(w.db, "SELECT rowid FROM Name2Id WHERE user_name=" +
                                 quote(a->myId) + " LIMIT 1");
    if (names.empty()) {
      writeBound(w.db, "INSERT INTO Name2Id(user_name) VALUES(?)",
                 json::array({a->myId}));
      sender = sqlite3_last_insert_rowid(w.db);
    } else
      sender = number(names[0], "rowid");
    if (created <= 0)
      created = std::time(nullptr);
    json values = {{"local_id", nextId},
                   {"server_id", 0},
                   {"local_type", 1},
                   {"sort_seq", sort},
                   {"real_sender_id", sender},
                   {"create_time", created},
                   {"message_content", text(content)},
                   {"compress_content", ""},
                   {"status", status}};
    auto columns = columnNames(w.db, table);
    std::string namesSql, bindings;
    json cells = json::array();
    for (auto i = values.begin(); i != values.end(); ++i)
      if (columns.count(i.key())) {
        if (!namesSql.empty()) {
          namesSql += ",";
          bindings += ",";
        }
        namesSql += identifier(i.key());
        bindings += "?";
        cells.push_back(i.value());
      }
    writeBound(w.db,
               "INSERT INTO " + identifier(table) + "(" + namesSql +
                   ") VALUES(" + bindings + ")",
               cells);
    w.commit();
    bool sessionUpdated = false;
    {
      Writer s(a, a->session);
      auto columns = columnNames(s.db, "SessionTable");
      std::string sql =
          "UPDATE SessionTable SET summary=?,last_timestamp=?,sort_timestamp=?";
      if (columns.count("last_msg_type"))
        sql += ",last_msg_type=1";
      writeBound(s.db, sql + " WHERE username=?",
                 json::array({text(content), created, created, session}));
      sessionUpdated = sqlite3_changes(s.db) > 0;
      s.commit();
    }
    return json{{"success", true},
                {"local_id", nextId},
                {"create_time", created},
                {"sort_seq", sort},
                {"status", status},
                {"sender_row_id", sender},
                {"session_updated", sessionUpdated},
                {"db_path", target.u8string()},
                {"table_name", table}};
  });
}
API int32_t wcdb_check_message_anti_revoke_trigger(int64_t h, const char *sid,
                                                   int32_t *installed) {
  return guarded([&]() {
    if (!installed)
      return -1;
    *installed = 0;
    auto a = account(h);
    for (auto &p : a->files("message"))
      if (!query(
               a->database(p),
               "SELECT name FROM sqlite_master WHERE type='trigger' AND name=" +
                   quote(messageTrigger(text(sid))))
               .empty())
        *installed = 1;
    return 0;
  });
}
API int32_t wcdb_install_message_anti_revoke_trigger(int64_t h, const char *sid,
                                                     void **out) {
  return writeErrorResult(out, [&]() {
    auto a = account(h);
    int count = 0;
    for (auto &p : a->files("message"))
      if (hasTable(a->database(p), "Msg_" + md5(text(sid)))) {
        Writer w(a, p);
        installMessageTriggers(w, text(sid));
        w.commit();
        ++count;
      }
    if (!count)
      throw std::runtime_error("Conversation table was not found");
    return json{{"success", true}, {"tables", count}};
  });
}
API int32_t wcdb_uninstall_message_anti_revoke_trigger(int64_t h,
                                                       const char *sid,
                                                       void **out) {
  return writeErrorResult(out, [&]() {
    auto a = account(h);
    for (auto &p : a->files("message"))
      if (hasTable(a->database(p), "Msg_" + md5(text(sid)))) {
        Writer w(a, p);
        w.exec("DROP TRIGGER IF EXISTS " +
               identifier(messageTrigger(text(sid))));
        w.exec("DROP TRIGGER IF EXISTS " +
               identifier(messageTrigger(text(sid), "delete")));
        w.commit();
      }
    return json{{"success", true}};
  });
}
static void snsTriggers(Writer &w) {
  w.exec("CREATE TRIGGER IF NOT EXISTS weflow_keep_sns_delete BEFORE DELETE ON "
         "SnsTimeLine BEGIN SELECT RAISE(IGNORE); END");
  w.exec("CREATE TRIGGER IF NOT EXISTS weflow_keep_sns_update BEFORE UPDATE ON "
         "SnsTimeLine WHEN length(OLD.content)>0 AND (NEW.content IS NULL OR "
         "length(NEW.content)=0) BEGIN SELECT RAISE(IGNORE); END");
}
API int32_t wcdb_check_sns_block_delete_trigger(int64_t h, int32_t *installed) {
  return guarded([&]() {
    if (!installed)
      return -1;
    auto a = account(h);
    auto path = optionalDb(a, "sns");
    *installed = !path.empty() && !query(a->database(path),
                                         "SELECT name FROM sqlite_master WHERE "
                                         "name='weflow_keep_sns_delete'")
                                       .empty()
                     ? 1
                     : 0;
    return 0;
  });
}
API int32_t wcdb_install_sns_block_delete_trigger(int64_t h, void **out) {
  return writeErrorResult(out, [&]() {
    auto a = account(h);
    Writer w(a, a->resolve("sns", ""));
    snsTriggers(w);
    w.commit();
    return json{{"success", true}};
  });
}
API int32_t wcdb_uninstall_sns_block_delete_trigger(int64_t h, void **out) {
  return writeErrorResult(out, [&]() {
    auto a = account(h);
    Writer w(a, a->resolve("sns", ""));
    w.exec("DROP TRIGGER IF EXISTS weflow_keep_sns_delete");
    w.exec("DROP TRIGGER IF EXISTS weflow_keep_sns_update");
    w.commit();
    return json{{"success", true}};
  });
}
API int32_t wcdb_delete_sns_post(int64_t h, const char *id, void **out) {
  return writeErrorResult(out, [&]() {
    auto value = text(id);
    if (!std::regex_match(value, std::regex("-?[0-9]+")))
      throw std::runtime_error("Invalid post ID");
    auto a = account(h);
    Writer w(a, a->resolve("sns", ""));
    bool protectedTable = !query(w.db, "SELECT name FROM sqlite_master WHERE "
                                       "name='weflow_keep_sns_delete'")
                               .empty();
    if (protectedTable)
      w.exec("DROP TRIGGER weflow_keep_sns_delete");
    auto deleted =
        writeBound(w.db, "DELETE FROM SnsTimeLine WHERE tid=?",
                   json::array({static_cast<int64_t>(std::stoull(value))}));
    if (protectedTable)
      snsTriggers(w);
    w.commit();
    return json{{"success", true}, {"deleted", deleted}};
  });
}
API int32_t wcdb_export_table_snapshot(int64_t h, const char *kind,
                                       const char *hint, const char *table,
                                       const char *outputPath, void **out) {
  return writeResult(out, [&]() {
    auto a = account(h);
    auto db = a->database(a->resolve(text(kind), text(hint)));
    auto name = text(table);
    auto schema =
        query(db, "SELECT sql FROM sqlite_master WHERE type='table' AND name=" +
                      quote(name));
    if (schema.empty())
      throw std::runtime_error("Table was not found");
    auto columns = tableColumns(db, name);
    json names = json::array();
    for (auto &c : columns)
      names.push_back(c["name"]);
    auto destination = fs::u8path(text(outputPath));
    if (destination.empty())
      throw std::runtime_error("Output path is required");
    if (!destination.parent_path().empty())
      fs::create_directories(destination.parent_path());
    if (fs::exists(destination))
      throw std::runtime_error("Snapshot destination already exists");
    std::ofstream file(destination, std::ios::binary);
    if (!file)
      throw std::runtime_error("Cannot create snapshot file");
    file << json({{"format", "weflow.table-snapshot.v1"},
                  {"table", name},
                  {"schema", schema[0]["sql"]},
                  {"columns", names}})
                .dump()
         << '\n';
    sqlite3_stmt *raw = nullptr;
    auto sql = "SELECT * FROM " + identifier(name);
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
      throw std::runtime_error(sqlite3_errmsg(db));
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(
        raw, sqlite3_finalize);
    int64_t count = 0;
    int status;
    while ((status = sqlite3_step(raw)) == SQLITE_ROW) {
      json cells = json::array();
      for (int i = 0; i < sqlite3_column_count(raw); ++i) {
        switch (sqlite3_column_type(raw, i)) {
        case SQLITE_NULL:
          cells.push_back(nullptr);
          break;
        case SQLITE_INTEGER:
          cells.push_back(
              {{"$int64", std::to_string(sqlite3_column_int64(raw, i))}});
          break;
        case SQLITE_FLOAT:
          cells.push_back(sqlite3_column_double(raw, i));
          break;
        case SQLITE_BLOB:
          cells.push_back({{"$blob", hex(static_cast<const unsigned char *>(
                                             sqlite3_column_blob(raw, i)),
                                         sqlite3_column_bytes(raw, i))}});
          break;
        default:
          cells.push_back(std::string(
              reinterpret_cast<const char *>(sqlite3_column_text(raw, i)),
              sqlite3_column_bytes(raw, i)));
        }
      }
      file << json({{"values", cells}})
                  .dump(-1, ' ', false, json::error_handler_t::replace)
           << '\n';
      ++count;
      if (!file)
        throw std::runtime_error("Snapshot write failed");
    }
    if (status != SQLITE_DONE)
      throw std::runtime_error(sqlite3_errmsg(db));
    file.close();
    if (!file)
      throw std::runtime_error("Snapshot close failed");
    return json{{"success", true}, {"rows", count}, {"columns", names.size()}};
  });
}
static json importSnapshot(std::shared_ptr<Account> a, const std::string &kind,
                           const std::string &hint, const std::string &table,
                           const std::string &inputPath,
                           const std::string &explicitSchema = "") {
  std::ifstream file(fs::u8path(inputPath), std::ios::binary);
  if (!file)
    throw std::runtime_error("Snapshot could not be opened");
  std::string line;
  if (!std::getline(file, line))
    throw std::runtime_error("Snapshot is empty");
  auto header = json::parse(line);
  if (str(header, "format") != "weflow.table-snapshot.v1" ||
      !header.contains("columns"))
    throw std::runtime_error("Unsupported snapshot format");
  Writer w(a, a->resolve(kind, hint), true);
  if (!explicitSchema.empty()) {
    if (!std::regex_search(explicitSchema, std::regex("^\\s*CREATE\\s+TABLE\\s",
                                                      std::regex::icase)))
      throw std::runtime_error("Expected a CREATE TABLE schema");
    if (!hasTable(w.db, table))
      w.exec(explicitSchema);
  }
  if (!hasTable(w.db, table))
    throw std::runtime_error("Destination table does not exist");
  auto actual = columnNames(w.db, table);
  std::string columns, parameters;
  for (auto &c : header["columns"]) {
    auto name = c.get<std::string>();
    if (!actual.count(name))
      throw std::runtime_error("Snapshot column is absent from destination");
    if (!columns.empty()) {
      columns += ",";
      parameters += ",";
    }
    columns += identifier(name);
    parameters += "?";
  }
  int64_t rows = 0, inserted = 0, ignored = 0, malformed = 0;
  while (std::getline(file, line)) {
    if (line.empty())
      continue;
    ++rows;
    json entry;
    try {
      entry = json::parse(line);
    } catch (...) {
      ++malformed;
      continue;
    }
    auto cells = entry.value("values", json::array());
    if (cells.size() != header["columns"].size()) {
      ++malformed;
      continue;
    }
    auto changed =
        writeBound(w.db,
                   "INSERT OR IGNORE INTO " + identifier(table) + "(" +
                       columns + ") VALUES(" + parameters + ")",
                   cells);
    inserted += changed;
    if (!changed)
      ++ignored;
  }
  if (file.bad())
    throw std::runtime_error("Snapshot read failed");
  w.commit();
  return {{"success", true},        {"rows", rows},
          {"inserted", inserted},   {"ignored", ignored},
          {"malformed", malformed}, {"columns", header["columns"].size()}};
}
API int32_t wcdb_import_table_snapshot(int64_t h, const char *kind,
                                       const char *hint, const char *table,
                                       const char *inputPath, void **out) {
  return writeResult(out, [&]() {
    return importSnapshot(account(h), text(kind), text(hint), text(table),
                          text(inputPath));
  });
}
API int32_t wcdb_import_table_snapshot_with_schema(
    int64_t h, const char *kind, const char *hint, const char *table,
    const char *inputPath, const char *schema, void **out) {
  return writeResult(out, [&]() {
    return importSnapshot(account(h), text(kind), text(hint), text(table),
                          text(inputPath), text(schema));
  });
}
