import { build } from 'vite';
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
process.env.VITE_DEVICE = '1';
const output = resolve(root, 'dist/device');
await build({root, build:{outDir:output, emptyOutDir:true, sourcemap:false, assetsInlineLimit:1000000}});
let html = (await readFile(resolve(output,'index.html'),'utf8')).replace(/\r\n?/g, '\n');
html = html.replace(' · 桌面预览',' · 实机调试');
for (const match of [...html.matchAll(/<script\b[^>]*src="([^"]+)"[^>]*><\/script>/g)]) {
  const js = await readFile(resolve(output,match[1].replace(/^\//,'')),'utf8');
  html = html.replace(match[0], () => `<script type="module">${js.replace(/<\/script/gi,'<\\/script')}</script>`);
}
for (const match of [...html.matchAll(/<link\b[^>]*href="([^"]+\.css)"[^>]*>/g)]) {
  html = html.replace(match[0], () => 'CSS_PLACEHOLDER');
  const css = await readFile(resolve(output,match[1].replace(/^\//,'')),'utf8');
  html = html.replace('CSS_PLACEHOLDER', () => `<style>${css}</style>`);
}
html = html.replace(/<link\b[^>]*rel="modulepreload"[^>]*>/g,'');
html = html.replace(/\r\n?/g, '\n');
if (/<(?:script|link)\b[^>]*(?:src|href)="\//.test(html)) throw Error('External asset remains in device HTML');
const target=resolve(root,'../../device-controller/data/index.html');
await mkdir(dirname(target),{recursive:true});
await writeFile(target,html);
await writeFile(resolve(output,'index.html'),html);
console.log(`Embedded ${Buffer.byteLength(html)} bytes, SHA256 ${createHash('sha256').update(html).digest('hex')}`);
