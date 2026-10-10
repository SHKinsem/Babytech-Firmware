#pragma once

namespace babytech { namespace brain {
constexpr char kBrainPortalPage[] = R"HTML(<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1"><meta charset="utf-8">
<title>Babytech Brain Setup</title><style>
body{font:16px system-ui;max-width:540px;margin:24px auto;padding:16px}input{box-sizing:border-box;width:100%;padding:10px;margin:6px 0 14px}button{padding:10px;margin:6px 0}small{display:block;color:#555}section,details{border:1px solid #ccc;padding:16px;margin:16px 0}#result{white-space:pre-wrap}
</style></head><body><h1>Brain Network Setup</h1>
<small>Local temporary AP only. No reset, erase, installation or feeding commands.</small>
<section>Device ID<input id="device" readonly><button type="button" onclick="copyValue('device')">Copy Device ID</button>
<p>Development pairing code</p><input id="code" readonly><button type="button" onclick="copyValue('code')">Copy development code</button>
<small>Development code only, derived from the installed Device ID. Cloud may have rotated or disabled it; this is not the authoritative stored Cloud pairing code. Uninstalled/nonstandard IDs have no derived code.</small></section>
<section><h2>Wi-Fi</h2><form id="wifi"><label>SSID<input name="ssid" id="ssid" required maxlength="32"></label>
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
let initial=true;
async function copyValue(id){const e=document.getElementById(id);if(!e.value)return;try{if(!navigator.clipboard)throw new Error();await navigator.clipboard.writeText(e.value);document.getElementById('result').textContent='Copied';}catch(_){e.focus();e.select();let ok=false;try{ok=document.execCommand('copy');}catch(_){}document.getElementById('result').textContent=ok?'Copied':'Selected: use the browser Copy action';}}
async function status(){try{const r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)return;const s=await r.json();if(initial){for(const [id,key] of [['device','device_id'],['code','development_code'],['ssid','ssid'],['host','host'],['port','port'],['user','user']])document.getElementById(id).value=s[key]||'';initial=false;}document.getElementById('result').textContent=s.result+' | Wi-Fi '+(s.wifi_connected?'joined':'not joined')+' | MQTT '+(s.mqtt_connected?'ready':'not ready')+(s.feeding?' | Save blocked: feeding active':'');}catch(_){document.getElementById('result').textContent='AP unavailable or closed. Check Wi-Fi connection; hold BOOT for 5 seconds to reopen if needed.';}}
for(const kind of ['wifi','mqtt'])document.getElementById(kind).addEventListener('submit',async e=>{e.preventDefault();try{const r=await fetch('/api/'+kind,{method:'POST',headers:{'X-Babytech-Portal':'1','Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(e.target))});const s=await r.json();document.getElementById('result').textContent=s.result;e.target.querySelector('input[type=password]').value='';}catch(_){document.getElementById('result').textContent='Save response unavailable; inspect status, do not assume success.';}});
status();setInterval(status,1000);
</script></body></html>)HTML";
} }
