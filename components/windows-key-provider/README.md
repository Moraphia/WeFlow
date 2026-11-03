Local adapter for WeFlow's external key-provider NDJSON protocol.

The included KeyService source originates from cc/hicccc77's official WeFlow commit `fa4edebed63843f6383233a678bf27f327445c79` (2026-06-29). Original project: https://github.com/hicccc77/WeFlow . The original CC BY-NC-SA 4.0 license is included as LICENSE. The new launcher, NDJSON adapter and Node runtime shim are local modifications and use the same license. No endorsement by the original author is implied.

Build from the project root: `node components/windows-key-provider/build.cjs`.

Output: `resources/key/win32/x64/weflow-key-provider.exe` and `key-provider.cjs`. It uses the installed 64-bit Node.js runtime and the project's Koffi dependency. `--health` validates native exports without reading any WeChat data. Normal use accepts one JSON request on stdin and emits NDJSON on stdout. Credentials are returned only through the requested result protocol; diagnostic console output is suppressed.

This source snapshot does not include wx_key.dll. Supply a compatible library separately beside the generated adapter before using --health or key operations. The launcher build alone does not provide the native implementation.
