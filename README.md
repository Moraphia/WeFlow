# WeFlow 本地修改版源码

本仓库保存基于 [cc / hicccc77 的 WeFlow](https://github.com/hicccc77/WeFlow) 的本地修改源码。
上游基线为 `b4d334d6accc7b4d92cb0c54124eaf67d61e9807`，由 Moraphia 于 2026-11-03 整理发布。
原作者、应用信息和原许可证保留；此仓库不代表原作者发布或背书。

## 本地修改

- 独立 Windows WCDB 组件源码与合成数据库测试，支持查询、统计、媒体索引、明确调用的写入接口和快照。
- 导出 Worker 的可选 `nativeExportDirect` 路径，复用应用已有格式与媒体处理逻辑。
- Windows 密钥提供器的 NDJSON 适配源码，历史上游 KeyService 保留原署名和许可。
- 视频硬链接查找、朋友圈 XML/CDATA 解析、Windows Hello 句柄及本地启动图标修正。
- 本地依赖与锁文件调整。完整变更路径见 [SOURCE_SNAPSHOT.json](SOURCE_SNAPSHOT.json)。

## 从源码运行

需要 Node.js 24、npm 和适用平台的构建工具。

```bash
git clone https://github.com/Moraphia/WeFlow.git
cd WeFlow
npm ci
npm run typecheck
npm run test:wcdb
npm run dev
```

本仓库发布源码，不附带已编译原生组件、第三方解密插件或 Microsoft C++ 运行库。
聊天读取需要自行配置兼容组件和本人数据库权限；仅安装 npm 依赖不会自动取得这些能力。
组件接口见 [上游协议](docs/third-party-components.md)，本地实现构建方法见
[数据库组件](components/wcdb-provider/README.md) 和
[Windows 适配器](components/windows-key-provider/README.md)。
数据库组件在 Windows 使用 Visual Studio 2022 C++、Windows SDK、CMake，按构建脚本下载并校验
固定版本依赖。密钥适配器还需另行提供兼容 `wx_key.dll`，本仓库不分发该 DLL。

## 数据与许可证

个人配置、聊天数据、密钥和本机构建资料不随源码发布，详见 [PRIVACY.md](PRIVACY.md)。
根许可证为 [CC BY-NC-SA 4.0](LICENSE)，本地修改沿用相同许可，包含非商业和相同方式共享条件。
依赖各自保留原许可证，见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。

以下保留上游项目说明，其中下载链接与项目联系信息属于上游。

---

<p align="center">
  <img src="app.jpg" alt="WeFlow 应用预览" width="90%">
</p>

<h1 align="center">WeFlow</h1>

<p align="center">
  WeFlow 是一个<strong>完全本地</strong>的<strong>实时</strong>聊天记录查看、分析与导出工具。<br>
  它可以获取你的聊天记录并将其导出，还可以根据你的聊天记录为你生成独一无二的数据与年度报告。
</p>

<p align="center">
  <a href="https://github.com/hicccc77/WeFlow/stargazers"><img src="https://img.shields.io/github/stars/hicccc77/WeFlow?style=flat&label=Stars&labelColor=2A3B4C&color=60A5FA" alt="Stargazers"></a>
  <a href="https://github.com/hicccc77/WeFlow/network/members"><img src="https://img.shields.io/github/forks/hicccc77/WeFlow?style=flat&label=Forks&labelColor=2A3B4C&color=60A5FA" alt="Forks"></a>
  <a href="https://github.com/hicccc77/WeFlow/releases"><img src="https://img.shields.io/github/downloads/hicccc77/WeFlow/total?style=flat&label=Downloads&labelColor=2A3B4C&color=60A5FA" alt="Downloads"></a>
  <br><br>
  <a href="https://t.me/weflow_cc"><img src="https://img.shields.io/badge/Telegram-频道-60A5FA?style=flat&logo=telegram&logoColor=white&labelColor=2A3B4C&color=60A5FA" alt="Telegram Channel" style="height: 24px; vertical-align: middle;"></a>
  <a href="https://star-history.com/#hicccc77/WeFlow"><img src="https://api.star-history.com/badge?repo=hicccc77/WeFlow&theme=dark" alt="Star History Rank" style="height: 30px; vertical-align: middle;"></a>
</p>

> [!TIP]
> 需要配套的第三方可插拔组件（本项目依赖自定义的原生解密组件，需自行设计，见 [docs/third-party-components.md](docs/third-party-components.md)）


## 主要功能

- 本地实时查看聊天记录
- 朋友圈图片、视频、**实况**的预览和解密
- 统计分析与群聊画像
- 年度报告与可视化概览
- 导出聊天记录为 HTML 等格式
- HTTP API 接口（面向开发者）
- 查看完整能力清单：[详细功能](#详细功能清单)

## 支持平台与设备

| 平台 | 设备/架构 | 安装包 |
|------|----------|--------|
| Windows | Windows10+、x64 | `.exe` |
| macOS | Apple Silicon（M 系列，arm64） | `.dmg` |
| Linux | x64 设备（amd64） | `.AppImage`、`.tar.gz` |


## 详细功能清单

| 功能模块 | 说明 |
|---------|------|
| **聊天** | 解密聊天中的图片、视频、实况；支持**修改**本地消息 |
| **消息防撤回** | 防止其他人发送的消息被撤回 |
| **实时弹窗通知** | 新消息到达时提供桌面弹窗提醒，便于及时查看重要会话，提供黑白名单功能 |
| **私聊分析** | 统计好友间消息数量；分析消息类型与发送比例；查看消息时段分布等 |
| **群聊分析** | 查看群成员详细信息；分析群内发言排行、活跃时段和媒体内容 |
| **年度报告** | 生成按年统计的年度报告，或跨年度的长期历史报告 |
| **双人报告** | 选择指定好友，基于双方聊天记录生成专属分析报告 |
| **消息导出** | 将聊天记录导出为多种格式：JSON、HTML、Markdown、TXT、Excel、CSV、PGSQL、ChatLab专属格式等 |
| **朋友圈** | 解密朋友圈图片、视频、实况；导出朋友圈内容；拦截朋友圈的删除与隐藏操作； |
| **联系人** | 导出好友、群聊、公众号信息；找回部分曾经的好友 |
| **HTTP API 映射** | 将本地消息能力映射为 HTTP API，便于对接外部系统、自动化脚本与二次开发 |

## HTTP API

WeFlow 提供本地 HTTP API 服务，支持通过接口查询消息数据，可用于与其他工具集成或二次开发。

- **启用方式**：设置 → API 服务 → 启动服务
- **默认端口**：5031
- **访问地址**：`http://127.0.0.1:5031`
- **支持格式**：原始 JSON 或 [ChatLab](https://chatlab.fun/) 标准格式

完整接口文档：[点击查看](docs/HTTP-API.md)

## 面向开发者

如果你想从源码构建或为项目贡献代码，请遵循以下步骤：

```bash
# 1. 克隆项目到本地
git clone https://github.com/hicccc77/WeFlow.git
cd WeFlow

# 2. 安装项目依赖
npm install

# 3. 运行应用（开发模式）
npm run dev
```

## 推广与合作

如果您对 **WeFlow** 有兴趣，或者希望与我们展开深度合作或投放你的广告，欢迎随时通过邮件取得联系。我们非常期待与各位创作者、开发者及合作伙伴共同探索。

### 联系方式

欢迎发送邮件至：

<a href="mailto:yccccccy@proton.me"><img src="https://img.shields.io/badge/Email-yccccccy%40proton.me-60A5FA?style=flat-square&logo=proton&logoColor=white&labelColor=2A3B4C" alt="Protonmail" height="24px"></a>

## 合作伙伴

我们非常欢迎优秀的开源社区项目团队或其他团队与我们建立长期合作关系。期待与 WeFlow 携手并进，共同建设更开放的生态！

<p align="center">
  <!-- 是的你没看错这里还是占位！
  <a href="https://your-partner-website.com" target="_blank">
    <img src="https://via.placeholder.com/150x50?text=Partner+1+Logo" alt="Partner Name" width="150" style="margin: 10px; vertical-align: middle;" />
  </a> -->
</p>

---

## 贡献者

感谢所有做出贡献的开发者！

<p align="center">
  <a href="https://github.com/hicccc77/WeFlow/graphs/contributors">
    <img src="https://contrib.rocks/image?repo=hicccc77/WeFlow" alt="Contributors" />
  </a>
</p>

## Star History

<a href="https://www.star-history.com/#hicccc77/WeFlow&type=date&legend=top-left">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=hicccc77/WeFlow&type=date&theme=dark&legend=top-left" />
    <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=hicccc77/WeFlow&type=date&legend=top-left" />
    <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=hicccc77/WeFlow&type=date&legend=top-left" />
  </picture>
</a>

<div align="center">

---

**请负责任地使用本工具，遵守相关法律法规**

</div>
