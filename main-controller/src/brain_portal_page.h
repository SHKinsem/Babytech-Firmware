#pragma once

namespace babytech { namespace brain {
constexpr char kBrainPortalPage[] = R"HTML(<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1"><meta charset="utf-8">
<title>Babytech Brain Setup</title><style>
body{font:16px system-ui;max-width:540px;margin:24px auto;padding:16px}label{display:block}input,select{box-sizing:border-box;width:100%;min-width:0;padding:10px;margin:6px 0 14px;font:inherit}button{padding:10px;margin:6px 0;font:inherit}small{display:block;color:#555}section,details{border:1px solid #ccc;padding:16px;margin:16px 0}#result{white-space:pre-wrap}#device,#code{background:#f5f5f5}#scan-result{margin:6px 0 14px}
</style></head><body><h1>Brain Network Setup</h1>
<small>Local temporary AP only. No reset, erase, installation or feeding commands.</small>
<section><label>Device ID<input id="device" value="{{DEVICE_ID}}" readonly placeholder="Not installed"></label><button type="button" onclick="copyValue('device')">Copy Device ID</button>
<label>Pairing code<input id="code" value="{{PAIRING_CODE}}" readonly placeholder="Not available"></label><button type="button" onclick="copyValue('code')">Copy pairing code</button><small id="copy-result" role="status"></small>
<small>Default development code for this machine. If Cloud rotated or disabled it, use the code provided by Cloud instead. MQTT password is not the pairing code.</small></section>
<section><h2>Wi-Fi</h2><form id="wifi"><label>Network<select name="ssid" id="ssid" required><option value="">Select Wi-Fi</option></select></label>
<button type="button" id="refresh" onclick="scan(true)">Refresh networks</button><small id="scan-result" role="status">Scanning nearby 2.4 GHz networks...</small>
<details id="manual"><summary>Hidden network / enter manually</summary><label>Network name<input id="manual-ssid" name="ssid" maxlength="32" disabled></label></details>
<label>Password<input name="password" type="password" autocomplete="new-password" maxlength="64"></label>
<small>Passwords are never displayed. Enter the Wi-Fi password to save; empty means an open network.</small><button>Save Wi-Fi only</button></form></section>
<details><summary>Advanced MQTT settings</summary><form id="mqtt">
<label>Host<input name="host" id="host" value="101.33.219.108" required maxlength="127"></label>
<label>Port<input name="port" id="port" type="number" value="1883" min="1" max="65535" required></label>
<label>Username<input name="user" id="user" required maxlength="63"></label>
<label>Password<input name="password" type="password" autocomplete="new-password" required maxlength="127"></label>
<small>Existing custom settings are preserved and prefilled, except passwords. Enter the existing MQTT password when saving; saving does not rotate the server account.</small><button>Save MQTT only</button></form></details>
<p id="result">Loading...</p><small>Saved is not proof of connection/authentication. Automatic AP closes after Wi-Fi joins, without waiting for MQTT. A manually opened AP stays until a successful save and Wi-Fi connection. Reopen with BOOT held for 5 seconds after normal boot; USB configuration remains available.</small>
<script>
let initial=true,wifiEdited=false,savedSsid='',scanning=false;
const ssid=document.getElementById('ssid'),manual=document.getElementById('manual'),manualSsid=document.getElementById('manual-ssid');
ssid.addEventListener('change',()=>{wifiEdited=true;});manualSsid.addEventListener('input',()=>{wifiEdited=true;});
manual.addEventListener('toggle',()=>{ssid.disabled=manual.open;ssid.required=!manual.open;manualSsid.disabled=!manual.open;manualSsid.required=manual.open;});
function option(value,label){const e=document.createElement('option');e.value=value;e.textContent=label;return e;}
function keepNetwork(value){if(value&&!Array.from(ssid.options).some(e=>e.value===value))ssid.appendChild(option(value,value+' (saved / not in scan)'));}
async function copyValue(id){const e=document.getElementById(id),result=document.getElementById('copy-result');if(!e.value){result.textContent='Machine identity is not installed yet.';return;}try{if(!navigator.clipboard)throw new Error();await navigator.clipboard.writeText(e.value);result.textContent='Copied';}catch(_){e.focus();e.select();let ok=false;try{ok=document.execCommand('copy');}catch(_){}result.textContent=ok?'Copied':'Selected: use the browser Copy action';}}
async function scan(refresh=false){
  if(scanning)return;
  scanning=true;
  const button=document.getElementById('refresh'),result=document.getElementById('scan-result');
  button.disabled=true;result.textContent='Scanning nearby 2.4 GHz networks...';
  const controller=new AbortController(),timeout=setTimeout(()=>controller.abort(),30000);
  try{
    let s,first=true;
    do{
      const r=await fetch('/api/networks'+(refresh&&first?'?refresh=1':''),{cache:'no-store',signal:controller.signal});
      first=false;s=await r.json();
      if(r.status===202){await new Promise(resolve=>setTimeout(resolve,700));continue;}
      if(!r.ok)throw new Error();
      break;
    }while(true);
    const selected=ssid.value;
    ssid.replaceChildren(option('','Select Wi-Fi'));
    for(const n of s.networks)ssid.appendChild(option(n.ssid,n.ssid+(n.open?' (open)':'')));
    keepNetwork(selected||savedSsid);ssid.value=selected||(!wifiEdited?savedSsid:'');
    result.textContent=s.networks.length?'Select a network and enter its password.':'No networks found. Refresh or enter a hidden network manually.';
  }catch(_){result.textContent='Scan unavailable. Refresh to retry, or enter the network manually.';}
  finally{clearTimeout(timeout);scanning=false;button.disabled=false;}
}
async function status(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)return;const s=await r.json();document.getElementById('device').value=s.device_id||'';document.getElementById('code').value=s.development_code||'';if(initial){for(const [id,key] of [['host','host'],['port','port'],['user','user']])document.getElementById(id).value=s[key]||'';savedSsid=s.ssid||'';keepNetwork(savedSsid);if(!wifiEdited)ssid.value=savedSsid;initial=false;}document.getElementById('result').textContent=s.result+' | Wi-Fi '+(s.wifi_connected?'joined':'not joined')+' | MQTT '+(s.mqtt_connected?'ready':'not ready')+(s.feeding?' | Save blocked: feeding active':'');}catch(_){document.getElementById('result').textContent='AP unavailable or closed. Check Wi-Fi connection; hold BOOT for 5 seconds to reopen if needed.';}}
for(const kind of ['wifi','mqtt'])document.getElementById(kind).addEventListener('submit',async e=>{e.preventDefault();try{const r=await fetch('/api/'+kind,{method:'POST',headers:{'X-Babytech-Portal':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(e.target))});const s=await r.json();document.getElementById('result').textContent=s.result;e.target.querySelector('input[type=password]').value='';}catch(_){document.getElementById('result').textContent='Save response unavailable; inspect status, do not assume success.';}});
status();scan();setInterval(status,1000);
</script></body></html>)HTML";
} }
