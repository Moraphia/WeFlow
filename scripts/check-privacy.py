"""Review staged source without printing credential or identity matches."""
from pathlib import PurePosixPath
import subprocess, json, re, hashlib, collections, sys

PATTERNS = {'home-path': '(?i)(?:[A-Z]:[\\\\/]+Users[\\\\/]+[^\\\\/\\s\\"\\x27()\\[\\]{}|?*+]+|(?<![\\w./-])/Users/[^/\\s\\"\\x27()\\[\\]{}|?*+]+|(?<![\\w./-])/home/(?!runner\\b)[^/\\s\\"\\x27()\\[\\]{}|?*+]+)', 'wechat-id': '\\bwxid_(?!fixture\\b|fixture_|example\\b|example_|demo\\b|test\\b|sample\\b)[a-zA-Z0-9_-]{6,}', 'github-token': '\\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{30,})\\b', 'api-key': '\\b(?:sk-(?:proj-|svcacct-)?[A-Za-z0-9_-]{24,}|AKIA[A-Z0-9]{16}|AIza[A-Za-z0-9_-]{30,})\\b', 'private-key': '-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----', 'jwt': '\\beyJ[A-Za-z0-9_-]{12,}\\.[A-Za-z0-9_-]{12,}\\.[A-Za-z0-9_-]{12,}\\b', 'credential-url': 'https?://[^/\\s:@]+:[^/\\s@]+@', 'email': '[A-Za-z0-9_.+%-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}', 'literal-key': '(?i)(?:decryptKey|apiKey|accessToken|internalDbKeyHex|imageAesKey|password|aes_key)\\s*["\']?\\s*[:=]\\s*["\'][a-zA-Z0-9+/_=-]{16,}["\']'}
BLOCKED_DIRS={'.git','node_modules','.deployment','.verification','.tools','.sdk','.vscode','.idea',
 'deployment','release','dist','dist-electron','.next','target','build','build-tencent','vendor',
 'exports','scheduled-exports','logs','__pycache__','.cache'}
BLOCKED_EXT={'.db','.sqlite','.sqlite3','.jsonl','.log','.pid','.pyc','.exe','.dll','.node','.so',
 '.dylib','.zip','.7z','.apk','.aab','.jks','.keystore','.pfx','.p12','.key','.pem','.bak','.tmp'}

def git(*args,data=None):
 return subprocess.run(['git',*args],input=data,capture_output=True,check=True).stdout

def inspect_content(data):
 value=data.decode('utf-8')
 for rule,pattern in PATTERNS.items():
  for m in re.finditer(pattern,value):
   if rule=='email' and (m.group().endswith(('@example.com','@example.invalid','@users.noreply.github.com')) or '@example.' in m.group()):
    continue
   yield rule,hashlib.sha256(m.group().encode()).hexdigest(),value.count('\n',0,m.start())+1

def main():
 if '--self-test' in sys.argv:
  assert list(inspect_content(('ghp_'+'A'*36).encode()))
  assert list(inspect_content(('C:'+chr(92)+'Users'+chr(92)+'private-person'+chr(92)+'file').encode()))
  assert not list(inspect_content(b'contact@example.invalid'))
  print('PASS: synthetic credential, identity and placeholder checks')
  return 0
 rows=[]
 for entry in git('ls-files','--stage','-z').split(b'\0'):
  if entry:
   meta,name=entry.split(b'\t',1)
   mode,oid,stage=meta.decode().split()
   if stage!='0': raise RuntimeError('Unresolved index')
   rows.append((name.decode(),mode,oid))
 objects=git('cat-file','--batch',data=''.join(oid+'\n' for _,_,oid in rows).encode())
 blobs={};cursor=0
 for name,mode,oid in rows:
  end=objects.index(b'\n',cursor);header=objects[cursor:end].split()
  if header[0].decode()!=oid or header[1]!=b'blob': raise RuntimeError('Unexpected staged object')
  size=int(header[2]);cursor=end+1
  blobs[name]=objects[cursor:cursor+size];cursor+=size+1
 manifest=json.loads(blobs['.privacy/public-baseline.json'])
 allowed=collections.Counter((r['path'],r['rule'],r['digest']) for r in manifest['public_matches'])
 binary={r['path']:r['sha256'] for r in manifest['public_binary_assets']}
 errors=[]
 for name,mode,oid in rows:
  p=PurePosixPath(name);data=blobs[name]
  if mode not in {'100644','100755'} or p.is_absolute() or '..' in p.parts:
   errors.append((name,'unexpected-file-mode-or-path',0));continue
  if any(part in BLOCKED_DIRS for part in p.parts) or p.suffix.lower() in BLOCKED_EXT or p.name.startswith('.env') or re.search(r'\.db-(wal|shm|journal)$',name):
   errors.append((name,'private-or-generated-artifact',0));continue
  try:
   found=list(inspect_content(data))
  except UnicodeDecodeError:
   if hashlib.sha256(data).hexdigest()!=binary.get(name):
    errors.append((name,'unreviewed-binary',0))
   continue
  for rule,digest,line in found:
   key=(name,rule,digest)
   if allowed[key]: allowed[key]-=1
   else: errors.append((name,rule,line))
 if '--check-author' in sys.argv:
  if not re.search(rb'<[^<>]+@users\.noreply\.github\.com>',git('var','GIT_AUTHOR_IDENT')):
   errors.append(('[commit author]','use-github-noreply-email',0))
 for name,rule,line in errors: print(f'BLOCKED {name}:{line} [{rule}]')
 if errors: return 1
 print(f'PASS: reviewed {len(rows)} staged source files; private values withheld')
 return 0

if __name__=='__main__':
 try: sys.exit(main())
 except Exception as error:
  print('Privacy review failed: '+type(error).__name__,file=sys.stderr)
  sys.exit(1)
