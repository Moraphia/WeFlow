# 独立 WeFlow 数据库组件

按 [WeFlow 官方组件协议](https://github.com/hicccc77/WeFlow/blob/main/docs/third-party-components.md) 实现。使用腾讯官方 WCDB 2.1.16 中的 SQLCipher、Zstandard，无需已过期的 wcdb_api.dll 或官方 WeLive 二进制。

实现 91 个公开 WCDB 接口和 Windows Hello VerifyUser：账号、会话、联系人、群成员/群昵称、搜索、跨库游标、压缩文本、统计和报告、足迹及提及、媒体索引及语音分片、朋友圈、实时管道、消息编辑/插入/删除、保护触发器和无损快照。联系人分类遵循上游 TypeScript 回退规则。

普通查询使用只读连接，明确调用写入接口时才建立事务写连接。快照保持 BLOB、NULL、64 位整数，可恢复到缺失的加密数据库。防撤回及朋友圈保护需要用户在界面启用；防撤回不能找回启用前已消失的消息。

本地配置 nativeExportDirect=true，导出 Worker 直接使用此组件和应用已有媒体处理器，支持全部 10 种格式及五类附件。weflow_local_export.exe 另提供基础 raw-jsonl/text NDJSON 协议用于兼容调试；完整媒体导出请使用 WeFlow 界面。其他未启用直接导出的组件仍使用原 WeLive 协议。

Windows Hello 调用微软系统验证，必须由 Windows 实际批准；没有模拟成功。三个可选 wcdb_cloud_* 遥测接口未实现，本地聊天处理不依赖它们。

先退出 WeFlow，在项目根目录执行：

```powershell
./components/wcdb-provider/build.ps1
npm run typecheck
npx vite build
node components/wcdb-provider/test-exports.cjs
```

需要 Visual Studio 2022 C++、Windows SDK、CMake。构建脚本执行基础及功能测试；导出测试运行正式 Worker，检查全部格式与附件实际落盘。测试均使用合成加密数据库，结果位于 .deployment/*-test.json。真实账号检查只记录状态与数量。

源码包与 JSON 头文件固定版本并校验 SHA-256。产物位于 resources/local-wcdb/win32/x64/。依赖许可证随 THIRD-PARTY-NOTICES.txt 和 JSON-LICENSE.MIT 保留。

来源：[Tencent/wcdb v2.1.16](https://github.com/Tencent/wcdb/releases/tag/v2.1.16)、[nlohmann/json v3.12.0](https://github.com/nlohmann/json/tree/v3.12.0)。此独立实现不代表上游作者背书。
