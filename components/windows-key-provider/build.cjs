const esbuild = require('esbuild');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const output = path.join(root, 'resources/key/win32/x64');

esbuild.buildSync({
  entryPoints: [path.join(__dirname, 'provider.ts')],
  outfile: path.join(output, 'key-provider.cjs'),
  bundle: true,
  platform: 'node',
  format: 'cjs',
  target: 'node24',
  external: ['koffi'],
  alias: { electron: path.join(__dirname, 'electron-shim.cjs') },
});
const compiler = path.join(process.env.SystemRoot || 'C:\\Windows', 'Microsoft.NET/Framework64/v4.0.30319/csc.exe');
execFileSync(compiler, [
  '/nologo', '/optimize+', '/platform:x64', '/target:exe',
  '/out:' + path.join(output, 'weflow-key-provider.exe'),
  path.join(__dirname, 'Launcher.cs'),
], { stdio: 'inherit', windowsHide: true });
console.log('Built the local Windows key-provider adapter.');
