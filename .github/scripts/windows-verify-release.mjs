// Check a downloaded Windows release against the identity it publishes.
//
// windows-release.mjs proves, at publish time, that the installer, portable,
// source ZIP, build manifest and checksum file belong to one commit. This is
// the same statement checked from the other side: by whoever downloaded the
// five files, before a validation run records "tested build <commit>".
//
//   node .github/scripts/windows-verify-release.mjs --dir <downloaded files>
//        [--installed <install directory, straight after installing>]
//
// Prints a JSON report and exits 1 if anything disagrees. Reads only; needs
// nothing but Node.
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { inflateRawSync } from 'node:zlib';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');

// The central directory of a ZIP, enough to read entries and the archive
// comment (where `git archive` records the commit it packed). ZIP64 is
// refused, not guessed at: no release file is near 4 GB or 65535 entries.
export function readZip(bytes) {
  let end = -1;
  for (let at = bytes.length - 22; at >= Math.max(0, bytes.length - 22 - 0xffff); at--) {
    if (bytes.readUInt32LE(at) === 0x06054b50 && at + 22 + bytes.readUInt16LE(at + 20) === bytes.length) { end = at; break; }
  }
  if (end < 0) throw Error('Not a ZIP archive');
  const count = bytes.readUInt16LE(end + 10);
  let at = bytes.readUInt32LE(end + 16);
  if (count === 0xffff || at === 0xffffffff) throw Error('ZIP64 archives are not supported');
  const entries = new Map();
  for (let index = 0; index < count; index++) {
    if (bytes.readUInt32LE(at) !== 0x02014b50) throw Error('Damaged ZIP directory');
    const nameLength = bytes.readUInt16LE(at + 28);
    const entry = { method: bytes.readUInt16LE(at + 10), compressed: bytes.readUInt32LE(at + 20),
      size: bytes.readUInt32LE(at + 24), header: bytes.readUInt32LE(at + 42) };
    if (entry.compressed === 0xffffffff || entry.size === 0xffffffff || entry.header === 0xffffffff) throw Error('ZIP64 archives are not supported');
    // Old Compress-Archive versions wrote backslashes.
    const name = bytes.toString('utf8', at + 46, at + 46 + nameLength).replaceAll('\\', '/');
    if (!name.endsWith('/')) entries.set(name, entry);
    at += 46 + nameLength + bytes.readUInt16LE(at + 30) + bytes.readUInt16LE(at + 32);
  }
  return {
    comment: bytes.toString('utf8', end + 22),
    names: [...entries.keys()],
    read(name) {
      const entry = entries.get(name);
      if (!entry) return null;
      if (bytes.readUInt32LE(entry.header) !== 0x04034b50) throw Error('Damaged ZIP entry: ' + name);
      const start = entry.header + 30 + bytes.readUInt16LE(entry.header + 26) + bytes.readUInt16LE(entry.header + 28);
      const stored = bytes.subarray(start, start + entry.compressed);
      if (entry.method !== 0 && entry.method !== 8) throw Error('Unsupported ZIP compression in ' + name);
      const data = entry.method === 8 ? inflateRawSync(stored) : stored;
      if (data.length !== entry.size) throw Error('Damaged ZIP entry: ' + name);
      return data;
    },
  };
}

function inventory(folder, prefix = '') {
  const files = [];
  // Sorted, so two runs over the same directory report in the same order.
  for (const entry of readdirSync(folder, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
    if (entry.isDirectory()) files.push(...inventory(join(folder, entry.name), prefix + entry.name + '/'));
    else files.push(prefix + entry.name);
  }
  return files;
}

// Every disagreement is collected, so one run tells a tester all of what is
// wrong with a download instead of the first thing.
export function verifyRelease(dir, { installed } = {}) {
  const problems = [];
  const sumsFiles = readdirSync(dir).filter((name) => /^SpektraLab-.+-SHA256SUMS\.txt$/.test(name));
  if (sumsFiles.length !== 1) throw Error('Expected exactly one SpektraLab-<version>-SHA256SUMS.txt in ' + dir);
  const version = sumsFiles[0].slice('SpektraLab-'.length, -'-SHA256SUMS.txt'.length);
  if (!/^\d+\.\d+\.\d+(?:-(?:alpha|beta|rc)\.\d+)?$/.test(version)) throw Error('Unrecognised release version: ' + version);
  const base = 'SpektraLab-' + version;
  const names = { installer: base + '-windows-x64-setup.exe', portable: base + '-windows-x64-portable.zip',
    source: base + '-source.zip', build: base + '-BUILD.json' };

  // 1. The checksum file lists exactly the four other files, and they match.
  const listed = new Map();
  for (const line of readFileSync(join(dir, sumsFiles[0]), 'utf8').split(/\r?\n/).filter(Boolean)) {
    const match = /^([0-9a-f]{64}) [ *](.+)$/.exec(line);
    if (!match) problems.push('Unreadable checksum line: ' + line);
    else listed.set(match[2], match[1]);
  }
  const data = {}, hashes = {};
  for (const [role, name] of Object.entries(names)) {
    if (!listed.has(name)) problems.push('The checksum file does not list ' + name);
    if (!existsSync(join(dir, name))) { problems.push('Missing file: ' + name); continue; }
    data[role] = readFileSync(join(dir, name));
    hashes[name] = sha256(data[role]);
    if (listed.has(name) && listed.get(name) !== hashes[name]) problems.push('SHA-256 differs from the checksum file: ' + name);
  }
  for (const name of listed.keys()) if (!Object.values(names).includes(name)) problems.push('The checksum file lists an unexpected file: ' + name);

  // 2. The build manifest names this version, an independent Windows tag,
  //    one commit, and the same three packages.
  let build = null;
  if (data.build) {
    try { build = JSON.parse(data.build.toString('utf8')); } catch { problems.push('The build manifest is not JSON'); }
  }
  const commit = build && /^[0-9a-f]{40}$/.test(build.commit ?? '') ? build.commit : null;
  if (build) {
    if (build.version !== version) problems.push('The build manifest is for version ' + build.version);
    if (build.tag !== 'spektralab-windows-v' + version) problems.push('The build manifest names tag ' + build.tag);
    if (!commit) problems.push('The build manifest has no source commit');
    for (const role of ['installer', 'portable', 'source']) {
      if (build.assets?.[names[role]] !== hashes[names[role]]) problems.push('The build manifest records a different ' + names[role]);
    }
  }

  // 3. The portable says which commit and files it is, and contains exactly
  //    those files.
  let portable = null;
  if (data.portable) {
    try {
      const zip = readZip(data.portable);
      const root = names.portable.slice(0, -4) + '/';
      const manifestBytes = zip.read(root + 'BUILD-MANIFEST.json');
      if (!manifestBytes) problems.push('The portable has no BUILD-MANIFEST.json');
      else {
        portable = JSON.parse(manifestBytes.toString('utf8'));
        if (portable.version !== version) problems.push('The portable is version ' + portable.version);
        if (commit && portable.commit !== commit) problems.push('The portable was built from commit ' + portable.commit);
        const expected = new Set([root + 'BUILD-MANIFEST.json']);
        for (const file of portable.files) {
          expected.add(root + file.path);
          const bytes = zip.read(root + file.path);
          if (!bytes) problems.push('The portable lacks a file its manifest lists: ' + file.path);
          else if (bytes.length !== file.bytes || sha256(bytes) !== file.sha256) problems.push('A portable file differs from its manifest: ' + file.path);
        }
        for (const name of zip.names) if (!expected.has(name)) problems.push('The portable holds a file its manifest does not list: ' + name);
      }
    } catch (error) { problems.push('Cannot read the portable: ' + error.message); }
  }

  // 4. `git archive <commit>` writes that commit id as the ZIP comment.
  if (data.source) {
    try {
      const packed = readZip(data.source).comment.trim();
      if (commit && packed !== commit) problems.push('The source ZIP was archived from ' + (packed || 'an unrecorded commit'));
    } catch (error) { problems.push('Cannot read the source ZIP: ' + error.message); }
  }

  // 5. Optionally: what the installer laid down is what the portable holds.
  //    Run straight after installing; the uninstaller is the one file the
  //    portable leaves out.
  let compared;
  if (installed) {
    if (!portable) problems.push('Cannot compare the installed app without a readable portable manifest');
    else {
      const known = new Map(portable.files.map((file) => [file.path.toLowerCase(), file.sha256]));
      compared = 0;
      for (const path of inventory(installed)) {
        if (/^uninstall[^/]*\.exe$/i.test(path)) continue;
        const expected = known.get(path.toLowerCase());
        if (!expected) problems.push('Installed file is not in the portable: ' + path);
        else if (sha256(readFileSync(join(installed, path))) !== expected) problems.push('Installed file differs from the portable: ' + path);
        compared++;
      }
      if (!compared) problems.push('No installed files were found in ' + installed);
    }
  }

  return { ok: problems.length === 0, version, tag: 'spektralab-windows-v' + version, commit, run: build?.run ?? null,
    sha256: hashes, portable_files: portable?.files?.length ?? null, installed_files_compared: compared ?? null, problems };
}

function main() {
  const value = (flag) => { const at = process.argv.indexOf(flag); return at >= 0 ? process.argv[at + 1] : undefined; };
  const dir = value('--dir');
  if (!dir) throw Error('usage: windows-verify-release.mjs --dir <downloaded files> [--installed <install directory>]');
  const installed = value('--installed');
  const report = verifyRelease(resolve(dir), { installed: installed ? resolve(installed) : undefined });
  console.log(JSON.stringify(report, null, 2));
  if (!report.ok) process.exitCode = 1;
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) main();
