#pragma once

static const char kBabytechOtaPage[] = R"OTA(<!doctype html>
<html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Babytech · 固件升级</title>
<style>
body{font:16px/1.5 system-ui,sans-serif;max-width:660px;margin:6vh auto;padding:0 18px;color:#20352e;background:#f3f7f4}
section{background:#fff;border:1px solid #d6e2da;border-radius:14px;padding:22px;margin:20px 0}
h1{margin-bottom:4px}label{display:block;margin:16px 0}input[type=file],input[type=password]{display:block;margin-top:5px;width:100%;box-sizing:border-box;padding:9px}
button{background:#256d50;color:white;border:0;border-radius:8px;padding:11px 18px;font:inherit;cursor:pointer}button:disabled{opacity:.5}
.muted{color:#5f7168}.danger{color:#9a2e2e}progress{width:100%;height:16px}
</style>
<main><h1>固件升级</h1><p class="muted">仅上传与此板卡匹配的已签名固件包。升级期间网页和板间通信可能中断。</p>
<section><div id="identity">正在读取板卡信息…</div><p id="state" role="status"></p></section>
<section><form id="form">
<label>发布清单（manifest.json）<input id="manifest" type="file" accept=".json,application/json" required></label>
<label>固件（firmware.bin）<input id="firmware" type="file" accept=".bin,application/octet-stream" required></label>
<label>管理员代码（通过 USB 串口发送 OTA CODE 获取）<input id="admin" type="password" autocomplete="off" required></label>
<label><input id="isolated" type="checkbox" required> 已停机并切断电机动力电源</label>
<button id="submit" type="submit" disabled>校验并升级</button></form>
<progress id="progress" max="100" value="0"></progress><p id="result" role="status"></p></section>
<p class="muted"><a href="/">返回控制页面</a> · 首次启用 OTA 仍需经 USB 安装固件。</p></main>
<script>
const $=id=>document.getElementById(id), enc=new TextEncoder();
const K=[0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];
const rotr=(x,n)=>(x>>>n)|(x<<(32-n));
function sha256(input){
 const bytes=input instanceof Uint8Array?input:new Uint8Array(input), len=bytes.length;
 const padded=new Uint8Array((len+9+63)&~63);padded.set(bytes);padded[len]=128;
 const view=new DataView(padded.buffer),bits=BigInt(len)*8n;
 view.setUint32(padded.length-8,Number(bits>>32n));view.setUint32(padded.length-4,Number(bits&0xffffffffn));
 const h=[0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19],w=new Uint32Array(64);
 for(let offset=0;offset<padded.length;offset+=64){
  for(let i=0;i<16;i++)w[i]=view.getUint32(offset+i*4);
  for(let i=16;i<64;i++){const a=w[i-15],b=w[i-2];w[i]=(w[i-16]+(rotr(a,7)^rotr(a,18)^(a>>>3))+w[i-7]+(rotr(b,17)^rotr(b,19)^(b>>>10)))>>>0;}
  let [a,b,c,d,e,f,g,j]=h;
  for(let i=0;i<64;i++){const t1=(j+(rotr(e,6)^rotr(e,11)^rotr(e,25))+((e&f)^(~e&g))+K[i]+w[i])>>>0,t2=((rotr(a,2)^rotr(a,13)^rotr(a,22))+((a&b)^(a&c)^(b&c)))>>>0;j=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0;}
  h[0]=(h[0]+a)>>>0;h[1]=(h[1]+b)>>>0;h[2]=(h[2]+c)>>>0;h[3]=(h[3]+d)>>>0;h[4]=(h[4]+e)>>>0;h[5]=(h[5]+f)>>>0;h[6]=(h[6]+g)>>>0;h[7]=(h[7]+j)>>>0;
 }
 const out=new Uint8Array(32),result=new DataView(out.buffer);h.forEach((v,i)=>result.setUint32(i*4,v));return out;
}
function concat(a,b){const out=new Uint8Array(a.length+b.length);out.set(a);out.set(b,a.length);return out;}
function hmac(key,message){let k=enc.encode(key);if(k.length>64)k=sha256(k);const inner=new Uint8Array(64),outer=new Uint8Array(64);inner.fill(0x36);outer.fill(0x5c);for(let i=0;i<k.length;i++){inner[i]^=k[i];outer[i]^=k[i];}return sha256(concat(outer,sha256(concat(inner,enc.encode(message)))));}
const hex=b=>Array.from(b,x=>x.toString(16).padStart(2,'0')).join('');
const manifestMessage=m=>`BABYTECH-OTA-V1\n${m.board}\n${m.hardware}\n${m.build}\n${m.version}\n${m.size}\n${m.sha256}\n`;
let current;
async function json(url,options){const r=await fetch(url,{cache:'no-store',...options});const d=await r.json();if(!r.ok)throw Error(d.error||('HTTP '+r.status));return d;}
async function refresh(){current=await json('/api/ota/status');$('identity').textContent=`${current.board} · ${current.hardware} · 当前版本 ${current.version}（build ${current.build}）`;$('state').textContent=`状态：${current.state}${current.error?' · '+current.error:''}`;$('submit').disabled=!current.enabled||current.state==='ready'||current.state==='uploading'||current.state==='rebooting';}
refresh().catch(e=>{$('identity').textContent='读取板卡失败：'+e.message;});
$('form').onsubmit=async event=>{
 event.preventDefault();$('submit').disabled=true;$('result').textContent='正在校验发布清单…';
 try{
  const manifest=JSON.parse(await $('manifest').files[0].text()),file=$('firmware').files[0];
  if(!current||manifest.board!==current.board||manifest.hardware!==current.hardware||manifest.size!==file.size||manifest.build<=current.build)throw Error('清单与板卡、版本或固件大小不匹配');
  const message=manifestMessage(manifest);
  const challenge=await json('/api/ota/challenge');
  const proof=hex(hmac($('admin').value.trim(),`BABYTECH-OTA-AUTH-V1\n${challenge.nonce}\n${message}`));
  const values=new URLSearchParams({...manifest,nonce:challenge.nonce,proof});
  const session=await json('/api/ota/session',{method:'POST',body:values});
  $('result').textContent='正在上传固件，请保持供电…';
  const form=new FormData();form.append('firmware',file,file.name);
  await new Promise((resolve,reject)=>{const xhr=new XMLHttpRequest();xhr.open('POST','/api/ota/image');xhr.setRequestHeader('X-OTA-Session',session.session);
   xhr.upload.onprogress=e=>{if(e.lengthComputable)$('progress').value=Math.round(e.loaded/e.total*100);};
   xhr.onload=()=>{let data={};try{data=JSON.parse(xhr.responseText);}catch{}xhr.status===200?resolve(data):reject(Error(data.error||('HTTP '+xhr.status)));};
   xhr.onerror=()=>reject(Error('网络中断，检查板卡是否仍可访问'));xhr.send(form);});
  current.state='rebooting';$('result').textContent='上传完成，板卡正在重启。重新连接热点后刷新页面确认版本。';
 }catch(e){$('result').textContent='升级失败：'+e.message;await refresh().catch(()=>{});}
 finally{if(current&&current.state!=='rebooting')$('submit').disabled=false;}
};
</script></html>)OTA";
