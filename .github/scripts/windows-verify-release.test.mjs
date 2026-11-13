import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve, sep } from 'node:path';
import { test } from 'node:test';
import { crc32, deflateRawSync } from 'node:zlib';
import { readZip, verifyRelease } from './windows-verify-release.mjs';

const sha256 = (bytes) => createHash('sha256').update(bytes).digest('hex');

// A small ZIP writer, so the fixtures need no tool the runner may lack and
// cover stored and deflated entries alike.
function zip(files, comment = '') {
  const locals = [], directory = [];
  let offset = 0;
  for (const [name, content] of Object.entries(files)) {
    const raw = Buffer.from(content), deflate = raw.length > 16, body = deflate ? deflateRawSync(raw) : raw;
    const nameBytes = Buffer.from(name), fields = Buffer.alloc(26);
    fields.writeUInt16LE(20, 0); fields.writeUInt16LE(0x0800, 2); fields.writeUInt16LE(deflate ? 8 : 0, 4);
    fields.writeUInt32LE(crc32(raw), 10); fields.writeUInt32LE(body.length, 14); fields.writeUInt32LE(raw.length, 18);
    fields.writeUInt16LE(nameBytes.length, 22);
    const local = Buffer.concat([Buffer.from([0x50, 0x4b, 0x03, 0x04]), fields, nameBytes, body]);
    const central = Buffer.alloc(46);
    central.writeUInt32LE(0x02014b50, 0); central.writeUInt16LE(20, 4); fields.copy(central, 6);
    central.writeUInt32LE(offset, 42);
    locals.push(local); directory.push(central, nameBytes);
    offset += local.length;
  }
  const directoryBytes = Buffer.concat(directory), commentBytes = Buffer.from(comment), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0); end.writeUInt16LE(locals.length, 8); end.writeUInt16LE(locals.length, 10);
  end.writeUInt32LE(directoryBytes.length, 12); end.writeUInt32LE(offset, 16); end.writeUInt16LE(commentBytes.length, 20);
  return Buffer.concat([...locals, directoryBytes, end, commentBytes]);
}

const version = '1.3.4-beta.3', commit = 'a'.repeat(40), other = 'c'.repeat(40);
const base = 'SpektraLab-' + version, portableRoot = base + '-windows-x64-portable/';
const app = { 'spektralab.exe': 'the application, long enough to be deflated in the archive',
  'spektralab-host.exe': 'the engine host, long enough to be deflated in the archive',
  'host/engine/spektrafilm_constants.bin': 'constants', 'README.txt': 'read me' };

// The five files exactly as windows-portable.mjs and windows-release.mjs
// write them, with one place per fixture to make something disagree.
function release(change = {}) {
  const files = { ...app, ...change.portableFiles };
  const manifest = { version, commit: change.portableCommit ?? commit,
    files: Object.entries(app).map(([path, content]) => ({ path, bytes: Buffer.byteLength(content), sha256: sha256(Buffer.from(content)) })) };
  const packed = Object.fromEntries(Object.entries(files).map(([path, content]) => [(change.backslashes ? portableRoot.replaceAll('/', '\\') + path.replaceAll('/', '\\') : portableRoot + path), content]));
  packed[portableRoot + 'BUILD-MANIFEST.json'] = JSON.stringify(manifest);
  const out = { [base + '-windows-x64-setup.exe']: Buffer.from('installer'),
    [base + '-windows-x64-portable.zip']: zip(packed),
    [base + '-source.zip']: zip({ 'SpektraLab/README.md': 'source' }, change.sourceCommit ?? commit) };
  const assets = Object.fromEntries(Object.entries(out).map(([name, bytes]) => [name, sha256(bytes)]));
  out[base + '-BUILD.json'] = Buffer.from(JSON.stringify({ version, tag: change.tag ?? 'spektralab-windows-v' + version, title: 'beta.3', commit,
    run: 'https://github.com/JamesQiu2005/SpektraLab/actions/runs/1', assets }, null, 2) + '\n');
  const sums = Object.entries(out).map(([name, bytes]) => sha256(bytes) + '  ' + name);
  out[base + '-SHA256SUMS.txt'] = Buffer.from([...sums, ...(change.extraSums ?? [])].join('\n') + '\n');
  return out;
}

function inTemp(run) {
  const root = mkdtempSync(join(tmpdir(), 'spk-verify-'));
  try { run(root); } finally {
    assert.ok(resolve(root).startsWith(resolve(tmpdir()) + sep + 'spk-verify-'));
    rmSync(root, { recursive: true });
  }
}
function write(root, files) {
  for (const [name, bytes] of Object.entries(files)) {
    mkdirSync(dirname(join(root, name)), { recursive: true });
    writeFileSync(join(root, name), bytes);
  }
}
const problemsOf = (files, after = () => {}) => {
  let problems;
  inTemp((root) => { write(root, files); after(root); problems = verifyRelease(root).problems; });
  return problems;
};

test('a ZIP reader returns stored and deflated entries and the archive comment', () => {
  const archive = readZip(zip({ 'a.txt': 'short', 'dir/b.txt': 'long enough to be deflated by the fixture writer' }, commit));
  assert.deepEqual(archive.names, ['a.txt', 'dir/b.txt']);
  assert.equal(archive.read('a.txt').toString(), 'short');
  assert.equal(archive.read('dir/b.txt').toString(), 'long enough to be deflated by the fixture writer');
  assert.equal(archive.read('missing'), null);
  assert.equal(archive.comment, commit);
  assert.throws(() => readZip(Buffer.from('not an archive, but longer than an end record')), /Not a ZIP/);
});

test('a complete release from one commit verifies and reports its identity', () => {
  inTemp((root) => {
    write(root, release());
    const report = verifyRelease(root);
    assert.deepEqual(report.problems, []);
    assert.equal(report.ok, true);
    assert.deepEqual([report.version, report.tag, report.commit, report.portable_files],
      [version, 'spektralab-windows-v' + version, commit, Object.keys(app).length]);
    assert.equal(Object.keys(report.sha256).length, 4);
  });
  assert.deepEqual(problemsOf(release({ backslashes: true })), []);
});

test('a file changed after publishing, or missing, is named', () => {
  assert.match(problemsOf(release(), (root) => writeFileSync(join(root, base + '-windows-x64-setup.exe'), 'tampered')).join('\n'),
    /SHA-256 differs from the checksum file: .*setup\.exe[\s\S]*build manifest records a different .*setup\.exe/);
  assert.match(problemsOf(release(), (root) => rmSync(join(root, base + '-source.zip'))).join('\n'), /Missing file: .*source\.zip/);
  assert.match(problemsOf(release({ extraSums: ['b'.repeat(64) + '  extra.exe'] })).join('\n'), /unexpected file: extra\.exe/);
});

test('packages from another commit or tag are refused', () => {
  assert.match(problemsOf(release({ portableCommit: other })).join('\n'), /portable was built from commit c{40}/);
  assert.match(problemsOf(release({ sourceCommit: other })).join('\n'), /source ZIP was archived from c{40}/);
  assert.match(problemsOf(release({ sourceCommit: '' })).join('\n'), /source ZIP was archived from an unrecorded commit/);
  assert.match(problemsOf(release({ tag: 'spektralab-v' + version })).join('\n'), /names tag spektralab-v/);
});

test('the portable must contain exactly what its manifest lists', () => {
  assert.match(problemsOf(release({ portableFiles: { 'spektralab.exe': 'a different application, also long enough to deflate' } })).join('\n'),
    /portable file differs from its manifest: spektralab\.exe/);
  assert.match(problemsOf(release({ portableFiles: { 'extra.dll': 'unlisted' } })).join('\n'), /does not list: .*extra\.dll/);
});

test('an installed app is compared with the portable, ignoring only the uninstaller', () => {
  inTemp((root) => {
    const dir = join(root, 'release'), installed = join(root, 'installed');
    write(dir, release());
    write(installed, { ...app, 'uninstall.exe': 'uninstaller' });
    const good = verifyRelease(dir, { installed });
    assert.deepEqual([good.problems, good.installed_files_compared], [[], Object.keys(app).length]);
    writeFileSync(join(installed, 'spektralab-host.exe'), 'another host');
    writeFileSync(join(installed, 'stray.dll'), 'stray');
    assert.match(verifyRelease(dir, { installed }).problems.join('\n'),
      /Installed file differs from the portable: spektralab-host\.exe[\s\S]*Installed file is not in the portable: stray\.dll/);
  });
});
