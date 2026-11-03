// Creates only synthetic encrypted databases for independent ABI tests.
#include <filesystem>
#include <iostream>
#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <zstd/zstd.h>
static std::string compressedHex(const std::string &input) {
  std::string buffer(ZSTD_compressBound(input.size()), '\0');
  auto size = ZSTD_compress(buffer.data(), buffer.size(), input.data(),
                            input.size(), 1);
  if (ZSTD_isError(size))
    throw std::runtime_error("Fixture compression failed");
  std::string result;
  for (size_t i = 0; i < size; ++i) {
    auto byte = static_cast<unsigned char>(buffer[i]);
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}
static void make(const std::filesystem::path &file, const std::string &sql) {
  std::filesystem::create_directories(file.parent_path());
  sqlite3 *db = nullptr;
  if (sqlite3_open(file.u8string().c_str(), &db) != SQLITE_OK)
    throw std::runtime_error("Fixture open failed");
  const std::string key =
      "x'0101010101010101010101010101010101010101010101010101010101010101'";
  sqlite3_key(db, key.c_str(), static_cast<int>(key.size()));
  sqlite3_exec(db,
               "PRAGMA cipher_compatibility=4; PRAGMA cipher_page_size=4096;",
               nullptr, nullptr, nullptr);
  char *error = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
    std::string message = error ? error : "Fixture SQL failed";
    sqlite3_free(error);
    sqlite3_close(db);
    throw std::runtime_error(message);
  }
  sqlite3_close(db);
}
int main(int argc, char **argv) {
  try {
    if (argc == 4 && std::string(argv[1]) == "--fixture-sql") {
      make(std::filesystem::u8path(argv[2]), argv[3]);
      return 0;
    }
    if (argc != 3 && argc != 5)
      return 2;
    auto storage = std::filesystem::u8path(argv[1]) / "db_storage";
    std::string table = "Msg_" + std::string(argv[2]);
    make(storage / "session/session.db",
         "CREATE TABLE SessionTable(username TEXT PRIMARY KEY,type "
         "INTEGER,summary TEXT,sort_timestamp INTEGER,last_timestamp "
         "INTEGER,unread_count INTEGER DEFAULT 2);INSERT INTO "
         "SessionTable(username,type,summary,sort_timestamp,last_timestamp) "
         "VALUES('fixture_friend',1,'hello',30,30);");
    make(storage / "contact/contact.db",
         "CREATE TABLE contact(username TEXT PRIMARY KEY,remark TEXT,nick_name "
         "TEXT,alias TEXT,local_type INTEGER DEFAULT 1,delete_flag INTEGER "
         "DEFAULT 0,flag INTEGER DEFAULT 0,big_head_url TEXT,small_head_url "
         "TEXT,extra_buffer BLOB);INSERT INTO "
         "contact(username,remark,nick_name,alias) "
         "VALUES('fixture_friend','Test "
         "Friend','Friend','friend'),('fixture_null',NULL,NULL,NULL);");
    auto schema = "CREATE TABLE Name2Id(user_name TEXT);INSERT INTO "
                  "Name2Id(rowid,user_name) "
                  "VALUES(1,'fixture_me'),(2,'fixture_friend'); CREATE TABLE " +
                  table +
                  "(local_id INTEGER PRIMARY KEY,server_id INTEGER,local_type "
                  "INTEGER,sort_seq INTEGER,real_sender_id INTEGER,create_time "
                  "INTEGER,message_content TEXT,compress_content TEXT);";
    make(storage / "message/message_0.db",
         schema + "INSERT INTO " + table +
             " VALUES(1,9223372036854775000,1,10,1,10,'first',''),(3,1003,1,30,"
             "2,30,X'" +
             compressedHex("第三条") + "','');");
    make(storage / "message/message_1.db",
         schema + "INSERT INTO " + table +
             " VALUES(2,1002,1,20,2,20,'second','');");
    if (argc == 5) {
      auto group = "Msg_" + std::string(argv[3]),
           media = "Msg_" + std::string(argv[4]);
      make(storage / "session/session.db",
           "INSERT INTO "
           "SessionTable(username,type,summary,sort_timestamp,last_timestamp) "
           "VALUES('fixture_group@chatroom',2,'group',1700000030,1700000020),('"
           "fixture_media',1,'media',1700000040,1700000040);");
      make(storage / "contact/contact.db",
           "UPDATE contact SET "
           "big_head_url='https://example.invalid/"
           "avatar',flag=268435456,extra_buffer=X'6001' WHERE "
           "username='fixture_friend';INSERT INTO contact(username,nick_name) "
           "VALUES('fixture_me','Me'),('fixture_group@chatroom','Group'),('"
           "fixture_media','Media');CREATE TABLE name2id(username TEXT PRIMARY "
           "KEY);INSERT INTO name2id(rowid,username) "
           "VALUES(1,'fixture_group@chatroom'),(2,'fixture_me'),(3,'fixture_"
           "friend');CREATE TABLE chatroom_member(room_id INTEGER,member_id "
           "INTEGER);INSERT INTO chatroom_member VALUES(1,2),(1,3);CREATE "
           "TABLE chat_room(username TEXT,owner TEXT,ext_buffer BLOB);INSERT "
           "INTO chat_room "
           "VALUES('fixture_group@chatroom','fixture_me',X'"
           "0a170a0e666978747572655f667269656e6412054275646479');");
      auto msgSchema = [](const std::string &name) {
        return "CREATE TABLE " + name +
               "(local_id INTEGER PRIMARY KEY,server_id INTEGER,local_type "
               "INTEGER,sort_seq INTEGER,real_sender_id INTEGER,create_time "
               "INTEGER,message_content TEXT,compress_content TEXT,source "
               "BLOB,status INTEGER DEFAULT 0);";
      };
      make(storage / "message/message_0.db",
           msgSchema(group) + msgSchema(media) + "INSERT INTO " + group +
               " VALUES(1,2001,1,1,2,1700000000,'hello "
               "@fixture_me','','<msgsource><atuserlist>fixture_me</"
               "atuserlist></"
               "msgsource>',0),(2,2002,1,2,1,1700000010,'reply','',NULL,0);"
               "INSERT INTO " +
               media +
               " VALUES(1,3001,3,1,2,1700000000,'<img "
               "md5=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"/"
               ">','',NULL,0),(2,3002,34,2,2,1700000010,'<voicemsg "
               "voicelength=\"1200\"/"
               ">','',NULL,0),(3,3003,43,3,1,1700000020,'<videomsg "
               "md5=\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"/"
               ">','',NULL,0),(4,3004,47,4,1,1700000030,'<emoji "
               "md5=\"cccccccccccccccccccccccccccccccc\" "
               "cdnurl=\"https://example.invalid/emoji\"/"
               ">','',NULL,0),(5,3005,25769803825,5,2,1700000040,'<appmsg><"
               "type>6</type><title>file.txt</title></appmsg>','',NULL,0);");
      make(storage / "media/media_0.db",
           "CREATE TABLE Name2Id(user_name TEXT);INSERT INTO "
           "Name2Id(rowid,user_name) VALUES(1,'fixture_media');CREATE TABLE "
           "VoiceInfo(chat_name_id INTEGER,create_time INTEGER,local_id "
           "INTEGER,svr_id INTEGER,voice_data BLOB,data_index TEXT);INSERT "
           "INTO VoiceInfo VALUES(1,1700000010,2,3002,X'232153494c4b', "
           "'0'),(1,1700000010,2,3002,X'5f5633','1');");
      make(
          storage / "hardlink/hardlink.db",
          "CREATE TABLE dir2id(username TEXT);INSERT INTO "
          "dir2id(rowid,username) VALUES(1,'fixture_dir'),(2,'2023-11');CREATE "
          "TABLE image_hardlink_info_v4(md5 TEXT,file_name TEXT,modify_time "
          "INTEGER,dir1 INTEGER,dir2 INTEGER);INSERT INTO "
          "image_hardlink_info_v4 "
          "VALUES('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa','fixture.dat',1,1,2);"
          "CREATE TABLE video_hardlink_info_v4(md5 TEXT,file_name "
          "TEXT,modify_time INTEGER,dir1 INTEGER,dir2 INTEGER);INSERT INTO "
          "video_hardlink_info_v4 "
          "VALUES('bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb','fixture.mp4',1,1,2);");
      make(
          storage / "head_image/head_image.db",
          "CREATE TABLE head_image(username TEXT PRIMARY KEY,image_buffer "
          "BLOB);INSERT INTO head_image VALUES('fixture_friend',X'ffd8ffd9');");
      make(storage / "emoticon/emoticon.db",
           "CREATE TABLE kNonStoreEmoticonTable(md5 TEXT,caption TEXT,cdn_url "
           "TEXT);INSERT INTO kNonStoreEmoticonTable "
           "VALUES('cccccccccccccccccccccccccccccccc','fixture "
           "emoji','https://example.invalid/emoji');");
      make(storage / "sns/sns.db",
           "CREATE TABLE SnsTimeLine(tid INTEGER PRIMARY KEY,user_name "
           "TEXT,content TEXT,pack_info_buf TEXT);INSERT INTO SnsTimeLine "
           "VALUES(-4186115273709551616,'fixture_me','<TimelineObject><id>"
           "14260633600000000000</id><username>fixture_me</"
           "username><createTime>1700000000</"
           "createTime><contentDesc><![CDATA[Fixture "
           "post]]></contentDesc><ContentObject><contentStyle>1</"
           "contentStyle><mediaList><media><url "
           "md5=\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\">https://example.invalid/"
           "photo</url></media></mediaList></ContentObject></"
           "TimelineObject>','<LocalExtraInfo><LikeUserList><LikeUser><"
           "username>fixture_friend</username><nickname>Friend</nickname></"
           "LikeUser></LikeUserList></LocalExtraInfo>');");
    }
    std::cout << "Synthetic encrypted fixture created\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
