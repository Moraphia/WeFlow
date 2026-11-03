# 来源与第三方许可

WeFlow 原作者为 cc / hicccc77，源自 https://github.com/hicccc77/WeFlow ，
基线提交见 SOURCE_SNAPSHOT.json。主项目与本地改动沿用根目录 CC BY-NC-SA 4.0。

Windows KeyService 源自上游历史提交 `fa4edebed63843f6383233a678bf27f327445c79`；
适配代码见 components/windows-key-provider，单独保留同一许可证。不附带其原生 DLL。

数据库组件从固定版本 Tencent WCDB 2.1.16 与 nlohmann/json 3.12.0 构建。
前者包含 BSD、SQLCipher、Zstandard、OpenSSL 等依赖许可；完整原文保留于
[组件声明](resources/local-wcdb/THIRD-PARTY-NOTICES.txt)，JSON 原文位于
[JSON-LICENSE.MIT](resources/local-wcdb/JSON-LICENSE.MIT)。下载地址和校验摘要在构建脚本中。
生成的程序及供应商源码未随本仓库分发。

上游已有图片、字体、图标和 wasm 素材按上游公开版本保留；npm 依赖按各自许可使用。
保留的原作者联系方式和版权信息属于公开署名，不是本机账号资料。
