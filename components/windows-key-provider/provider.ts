// Adapter for WeFlow's documented NDJSON key-provider protocol.
// The native implementation and KeyService are from cc/hicccc77's official historical commit.
import { readFileSync } from 'node:fs'
import { resolve } from 'node:path'
import { KeyService } from './upstream/keyService'

const emit = (value: unknown) => process.stdout.write(JSON.stringify(value) + '\n')
const redact = (value: unknown) => String(value ?? '').replace(/\b[0-9a-f]{64}\b/gi, '[redacted]')
console.log = () => {}
console.warn = () => {}
console.error = () => {}
process.env.WX_KEY_DLL_PATH = resolve(__dirname, 'wx_key.dll')

async function main() {
  if (process.argv.includes('--health')) {
    const koffi = require('koffi')
    const lib = koffi.load(process.env.WX_KEY_DLL_PATH)
    for (const signature of [
      'bool InitializeHook(uint32 targetPid)',
      'bool PollKeyData(_Out_ char* keyBuffer, int bufferSize)',
      'bool GetStatusMessage(_Out_ char* msgBuffer, int bufferSize, _Out_ int* outLevel)',
      'bool CleanupHook()',
      'const char* GetLastErrorMsg()',
      'bool GetImageKey(_Out_ char* resultBuffer, int bufferSize)',
    ]) lib.func(signature)
    emit({ type: 'result', success: true, mode: 'health', nativeExports: 6 })
    return
  }
  const input = readFileSync(0, 'utf8').trim()
  if (!input || input.length > 65536) throw new Error('Invalid provider request')
  const request = JSON.parse(input)
  const service = new KeyService()
  const progress = (message: string, level = 0) => emit({ type: 'progress', message: redact(message), level })
  let result: any
  switch (request.action) {
    case 'get_db_key':
      result = /^[0-9a-f]{64}$/i.test(String(request.internalDbKeyHex || ''))
        ? { success: true, key: String(request.internalDbKeyHex) }
        : await service.autoGetDbKey(Math.max(1000, Math.min(240000, Number(process.env.WEFLOW_KEY_TIMEOUT_MS || 55000))), progress)
      if (result.success && request.accountId) result.accountId = String(request.accountId)
      break
    case 'get_image_key':
      result = await service.autoGetImageKey(request.accountDir, progress, request.accountId)
      break
    case 'scan_image_key_memory':
      if (!request.accountDir) throw new Error('accountDir is required')
      result = await service.autoGetImageKeyByMemoryScan(String(request.accountDir), progress)
      break
    default:
      throw new Error('Unsupported provider action')
  }
  if (Array.isArray(result.logs)) result.logs = result.logs.map(redact)
  emit({ type: 'result', ...result })
}

main().then(() => process.exit(0)).catch(error => {
  emit({ type: 'result', success: false, error: redact(error?.message || error) })
  process.exit(1)
})
