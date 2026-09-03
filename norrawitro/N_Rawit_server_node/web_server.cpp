#include "web_server.h"
#include <esp_random.h>
#include <time.h>

// ─── Constructor ─────────────────────────────────────────
NRWebServer::NRWebServer(DBManager& dbm)
    : _server(WEBSERVER_PORT), _db(dbm) {
    memset(&_session, 0, sizeof(_session));
}

// ─── begin ───────────────────────────────────────────────
bool NRWebServer::begin() {
    // ตั้ง WiFi AP
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL, 0, WIFI_MAX_CLIENTS);
    Serial.printf("[WEB] AP started: %s  IP: %s\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str());

    // ─── Routes ─────────────────────────────────────────
    _server.on("/",          HTTP_GET,  [this]() { _handleRoot(); });
    _server.on("/login",     HTTP_POST, [this]() { _handleLogin(); });
    _server.on("/logout",    HTTP_GET,  [this]() { _handleLogout(); });
    _server.on("/dashboard", HTTP_GET,  [this]() { _handleDashboard(); });

    // API (ต้อง auth)
    _server.on("/api/list",   HTTP_GET,  [this]() { _handleApiList(); });
    _server.on("/api/meta",   HTTP_GET,  [this]() { _handleApiMeta(); });
    _server.on("/api/delete", HTTP_POST, [this]() { _handleApiDelete(); });
    _server.on("/api/upload", HTTP_POST, [this]() { _handleApiUpload(); });

    _server.onNotFound([this]() { _handleNotFound(); });

    _server.begin();
    Serial.printf("[WEB] Server started on port %d\n", WEBSERVER_PORT);
    return true;
}

// ─── handle ──────────────────────────────────────────────
void NRWebServer::handle() {
    _server.handleClient();
}

// ─── isAuthenticated ─────────────────────────────────────
bool NRWebServer::isAuthenticated(const String& token) {
    if (!_session.active) return false;
    if (token != _session.token) return false;
    uint32_t now = (uint32_t)time(nullptr);
    if (now - _session.loginTime > SESSION_TIMEOUT) {
        _session.active = false;
        return false;
    }
    return true;
}

// ─── _handleRoot ─────────────────────────────────────────
void NRWebServer::_handleRoot() {
    if (_checkAuth()) {
        _server.sendHeader("Location", "/dashboard");
        _server.send(302, "text/plain", "");
    } else {
        _server.send(200, "text/html", _pageLogin());
    }
}

// ─── _handleLogin ────────────────────────────────────────
void NRWebServer::_handleLogin() {
    String user = _server.arg("username");
    String pass = _server.arg("password");

    if (user == ADMIN_USERNAME && pass == ADMIN_PASSWORD) {
        // สร้าง session token
        _generateToken(_session.token);
        _session.active    = true;
        _session.loginTime = (uint32_t)time(nullptr);

        // ส่ง cookie + redirect
        String cookie = "NR_SESSION=";
        cookie += _session.token;
        cookie += "; Path=/; HttpOnly";
        _server.sendHeader("Set-Cookie", cookie);
        _server.sendHeader("Location", "/dashboard");
        _server.send(302, "text/plain", "");
        Serial.println("[WEB] Login success");
    } else {
        // Login fail
        String page = _pageLogin();
        page.replace("<!--ERROR-->",
            "<div class='err'>❌ รหัสผิด กรุณาลองใหม่</div>");
        _server.send(401, "text/html", page);
        Serial.println("[WEB] Login failed");
    }
}

// ─── _handleLogout ───────────────────────────────────────
void NRWebServer::_handleLogout() {
    _session.active = false;
    memset(_session.token, 0, sizeof(_session.token));
    _server.sendHeader("Set-Cookie", "NR_SESSION=; Path=/; Max-Age=0");
    _server.sendHeader("Location", "/");
    _server.send(302, "text/plain", "");
}

// ─── _handleDashboard ────────────────────────────────────
void NRWebServer::_handleDashboard() {
    if (!_checkAuth()) { _sendUnauthorized(); return; }
    _server.send(200, "text/html", _pageDashboard());
}

// ─── _handleApiList ──────────────────────────────────────
void NRWebServer::_handleApiList() {
    if (!_checkAuth()) { _server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }

    String json = "[";
    int count = _db.count();
    for (int i = 0; i < count; i++) {
        const DBEntry* e = _db.getByIndex(i);
        if (!e) continue;
        if (i > 0) json += ",";
        json += "{";
        json += "\"id\":\"" + String(e->id) + "\",";
        json += "\"type\":\"" + String(DBManager::typeName(e->type)) + "\",";
        json += "\"created\":" + String(e->created) + ",";
        json += "\"node_id\":" + String(e->node_id) + ",";
        json += "\"price\":" + String(e->price_token) + ",";
        json += "\"size\":" + String(e->payload_size) + ",";
        json += "\"rank\":" + String(e->rank_total);
        json += "}";
    }
    json += "]";
    _server.send(200, "application/json", json);
}

// ─── _handleApiMeta ──────────────────────────────────────
void NRWebServer::_handleApiMeta() {
    if (!_checkAuth()) { _server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }

    String id = _server.arg("id");
    const DBEntry* e = _db.findById(id.c_str());
    if (!e) {
        _server.send(404, "application/json", "{\"error\":\"not found\"}");
        return;
    }
    String meta = _db.readMeta(*e);
    _server.send(200, "application/json", meta);
}

// ─── _handleApiDelete ────────────────────────────────────
void NRWebServer::_handleApiDelete() {
    if (!_checkAuth()) { _server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }

    String id = _server.arg("id");
    if (_db.deleteById(id.c_str())) {
        _server.send(200, "application/json", "{\"ok\":true}");
    } else {
        _server.send(404, "application/json", "{\"error\":\"not found\"}");
    }
}

// ─── _handleApiUpload ────────────────────────────────────
void NRWebServer::_handleApiUpload() {
    if (!_checkAuth()) { _server.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }

    // รับ type, meta, payload จาก POST args
    String typeStr = _server.arg("type");
    String meta    = _server.arg("meta");
    String priceS  = _server.arg("price");

    uint8_t type = DB_TYPE_OTHER;
    if (typeStr == "IMG") type = DB_TYPE_IMG;
    else if (typeStr == "TXT") type = DB_TYPE_TXT;
    else if (typeStr == "VID") type = DB_TYPE_VID;
    else if (typeStr == "CFG") type = DB_TYPE_CFG;
    else if (typeStr == "PRG") type = DB_TYPE_PRG;

    uint16_t price = (uint16_t)priceS.toInt();

    // payload จาก body (text สำหรับ TXT)
    String body = _server.arg("payload");
    const uint8_t* payloadPtr = (const uint8_t*)body.c_str();
    uint32_t payloadLen = body.length();

    bool ok = _db.writeDB(type, meta.c_str(), payloadPtr, payloadLen, price, 0.0f, 0.0f);
    if (ok) {
        _server.send(200, "application/json", "{\"ok\":true}");
    } else {
        _server.send(500, "application/json", "{\"error\":\"write failed\"}");
    }
}

// ─── _handleNotFound ─────────────────────────────────────
void NRWebServer::_handleNotFound() {
    _server.send(404, "text/plain", "404 Not Found");
}

// ─── _sendUnauthorized ───────────────────────────────────
void NRWebServer::_sendUnauthorized() {
    _server.sendHeader("Location", "/");
    _server.send(302, "text/plain", "");
}

// ─── _checkAuth ──────────────────────────────────────────
bool NRWebServer::_checkAuth() {
    String token = _getSessionToken();
    return isAuthenticated(token);
}

// ─── _getSessionToken (จาก Cookie) ──────────────────────
String NRWebServer::_getSessionToken() {
    if (_server.hasHeader("Cookie")) {
        String cookie = _server.header("Cookie");
        int idx = cookie.indexOf("NR_SESSION=");
        if (idx >= 0) {
            int start = idx + 11;
            int end   = cookie.indexOf(';', start);
            if (end < 0) end = cookie.length();
            return cookie.substring(start, end);
        }
    }
    return "";
}

// ─── _generateToken ──────────────────────────────────────
void NRWebServer::_generateToken(char* out) {
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 63; i++) {
        out[i] = charset[esp_random() % (sizeof(charset) - 1)];
    }
    out[63] = '\0';
}

// ═══════════════════════════════════════════════════════════
// HTML PAGES
// ═══════════════════════════════════════════════════════════

// ─── _htmlHead ───────────────────────────────────────────
String NRWebServer::_htmlHead(const char* title) {
    return String(R"(<!DOCTYPE html><html lang="th"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>)") + title + R"(</title>
<style>
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Sarabun:wght@300;400;600&display=swap');
  :root{
    --bg:#0a0c10; --bg2:#111520; --bg3:#1a1f2e;
    --accent:#00e5ff; --accent2:#ff6b35; --accent3:#7fff6b;
    --text:#c8d8e8; --muted:#556070;
    --border:#1e2d40; --glow:0 0 12px rgba(0,229,255,0.3);
  }
  *{box-sizing:border-box;margin:0;padding:0}
  body{background:var(--bg);color:var(--text);
       font-family:'Sarabun',sans-serif;min-height:100vh}
  .mono{font-family:'Share Tech Mono',monospace}
</style>)";
}

// ─── _htmlFoot ───────────────────────────────────────────
String NRWebServer::_htmlFoot() {
    return "</body></html>";
}

// ─── _pageLogin ──────────────────────────────────────────
String NRWebServer::_pageLogin() {
    String h = _htmlHead("N_Rawit Box — Login");
    h += R"(
<style>
  body{display:flex;align-items:center;justify-content:center;
       background:var(--bg);overflow:hidden}
  .scan{position:fixed;top:0;left:0;width:100%;height:100%;
        background:repeating-linear-gradient(0deg,
          transparent,transparent 2px,rgba(0,229,255,0.03) 2px,rgba(0,229,255,0.03) 4px);
        pointer-events:none;z-index:0}
  .box{position:relative;z-index:1;width:340px;padding:40px 36px;
       background:var(--bg2);border:1px solid var(--border);
       border-top:2px solid var(--accent);}
  .logo{text-align:center;margin-bottom:32px}
  .logo .nr{font-family:'Share Tech Mono',monospace;font-size:28px;
             color:var(--accent);letter-spacing:4px;
             text-shadow:0 0 20px rgba(0,229,255,0.6)}
  .logo .sub{font-size:11px;color:var(--muted);letter-spacing:2px;margin-top:4px}
  .ver{font-size:10px;color:var(--muted);text-align:center;margin-bottom:28px;
       font-family:'Share Tech Mono',monospace}
  label{display:block;font-size:11px;letter-spacing:2px;color:var(--muted);
        text-transform:uppercase;margin-bottom:6px;margin-top:18px}
  input[type=text],input[type=password]{
    width:100%;padding:10px 14px;background:var(--bg3);
    border:1px solid var(--border);color:var(--text);
    font-family:'Share Tech Mono',monospace;font-size:14px;
    outline:none;transition:border .2s}
  input:focus{border-color:var(--accent);box-shadow:var(--glow)}
  .btn{margin-top:28px;width:100%;padding:12px;background:transparent;
       border:1px solid var(--accent);color:var(--accent);
       font-family:'Share Tech Mono',monospace;font-size:13px;
       letter-spacing:3px;cursor:pointer;text-transform:uppercase;
       transition:all .2s}
  .btn:hover{background:var(--accent);color:var(--bg);box-shadow:var(--glow)}
  .err{margin-top:16px;padding:10px 14px;background:rgba(255,107,53,0.1);
       border:1px solid var(--accent2);color:var(--accent2);
       font-size:13px;text-align:center}
  .node-id{text-align:center;margin-top:20px;font-family:'Share Tech Mono',monospace;
            font-size:10px;color:var(--muted)}
</style>
<div class="scan"></div>
<div class="box">
  <div class="logo">
    <div class="nr">N_RAWIT</div>
    <div class="sub">NODE CONTROL SYSTEM</div>
  </div>
  <div class="ver">v)";
    h += NODE_VERSION;
    h += R"( &nbsp;|&nbsp; NODE #)";
    h += String(NODE_ID);
    h += R"(</div>
  <form method="POST" action="/login">
    <label>Username</label>
    <input type="text" name="username" autocomplete="off" required>
    <label>Password</label>
    <input type="password" name="password" required>
    <!--ERROR-->
    <button class="btn" type="submit">[ เข้าสู่ระบบ ]</button>
  </form>
  <div class="node-id">)";
    h += WIFI_AP_SSID;
    h += R"(</div>
</div>)";
    h += _htmlFoot();
    return h;
}

// ─── _pageDashboard ──────────────────────────────────────
String NRWebServer::_pageDashboard() {
    String h = _htmlHead("N_Rawit Box — Dashboard");
    h += R"(
<style>
  body{display:flex;flex-direction:column;min-height:100vh}
  header{padding:14px 24px;background:var(--bg2);border-bottom:1px solid var(--border);
         display:flex;align-items:center;justify-content:space-between}
  .hlogo{font-family:'Share Tech Mono',monospace;color:var(--accent);
          font-size:16px;letter-spacing:3px}
  .hinfo{font-size:11px;color:var(--muted);font-family:'Share Tech Mono',monospace}
  .logout{padding:6px 14px;border:1px solid var(--muted);color:var(--muted);
           background:transparent;font-size:11px;cursor:pointer;
           font-family:'Share Tech Mono',monospace;letter-spacing:1px;
           transition:all .2s}
  .logout:hover{border-color:var(--accent2);color:var(--accent2)}
  main{flex:1;padding:24px;max-width:1100px;width:100%;margin:0 auto}
  .stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));
          gap:14px;margin-bottom:28px}
  .stat{background:var(--bg2);border:1px solid var(--border);
         border-left:3px solid var(--accent);padding:16px 20px}
  .stat-val{font-family:'Share Tech Mono',monospace;font-size:28px;color:var(--accent)}
  .stat-lbl{font-size:11px;color:var(--muted);letter-spacing:1px;margin-top:4px}
  .panel{background:var(--bg2);border:1px solid var(--border);margin-bottom:20px}
  .panel-hdr{padding:12px 20px;border-bottom:1px solid var(--border);
              display:flex;align-items:center;justify-content:space-between}
  .panel-hdr h2{font-family:'Share Tech Mono',monospace;font-size:13px;
                 color:var(--accent);letter-spacing:2px}
  .panel-body{padding:16px 20px}
  table{width:100%;border-collapse:collapse;font-size:13px}
  th{text-align:left;font-family:'Share Tech Mono',monospace;font-size:10px;
      color:var(--muted);letter-spacing:1px;padding:8px 10px;
      border-bottom:1px solid var(--border)}
  td{padding:10px 10px;border-bottom:1px solid rgba(30,45,64,0.5);vertical-align:middle}
  tr:hover td{background:rgba(0,229,255,0.03)}
  .badge{display:inline-block;padding:2px 8px;font-family:'Share Tech Mono',monospace;
          font-size:10px;letter-spacing:1px}
  .badge-IMG{background:rgba(127,255,107,0.1);color:var(--accent3);border:1px solid var(--accent3)}
  .badge-TXT{background:rgba(0,229,255,0.1);color:var(--accent);border:1px solid var(--accent)}
  .badge-VID{background:rgba(255,107,53,0.1);color:var(--accent2);border:1px solid var(--accent2)}
  .badge-CFG{background:rgba(255,220,0,0.1);color:#ffd700;border:1px solid #ffd700}
  .badge-PRG{background:rgba(200,100,255,0.1);color:#c864ff;border:1px solid #c864ff}
  .badge-OTH{background:rgba(100,100,100,0.1);color:var(--muted);border:1px solid var(--muted)}
  .del-btn{padding:4px 10px;background:transparent;border:1px solid var(--accent2);
            color:var(--accent2);font-size:11px;cursor:pointer;
            font-family:'Share Tech Mono',monospace;transition:all .2s}
  .del-btn:hover{background:var(--accent2);color:var(--bg)}
  .upload-form{display:grid;grid-template-columns:1fr 1fr;gap:14px}
  .upload-form .full{grid-column:1/-1}
  select,textarea,input[type=text],input[type=number]{
    width:100%;padding:8px 12px;background:var(--bg3);
    border:1px solid var(--border);color:var(--text);
    font-family:'Share Tech Mono',monospace;font-size:13px;outline:none}
  select:focus,textarea:focus,input:focus{border-color:var(--accent)}
  textarea{resize:vertical;min-height:80px}
  label.lbl{display:block;font-size:10px;letter-spacing:2px;color:var(--muted);
             text-transform:uppercase;margin-bottom:5px}
  .btn-submit{padding:10px 24px;background:transparent;border:1px solid var(--accent);
               color:var(--accent);font-family:'Share Tech Mono',monospace;font-size:12px;
               letter-spacing:2px;cursor:pointer;transition:all .2s}
  .btn-submit:hover{background:var(--accent);color:var(--bg)}
  #msg{margin-top:12px;font-size:13px;font-family:'Share Tech Mono',monospace;
        color:var(--accent3);min-height:20px}
  .empty{color:var(--muted);font-family:'Share Tech Mono',monospace;
          font-size:13px;padding:20px;text-align:center}
</style>

<header>
  <div class="hlogo">N_RAWIT_BOX</div>
  <div class="hinfo">)";
    h += NODE_NAME;
    h += R"( &nbsp;|&nbsp; NODE #)";
    h += String(NODE_ID);
    h += R"(</div>
  <button class="logout" onclick="location.href='/logout'">LOGOUT</button>
</header>

<main>
  <!-- Stats -->
  <div class="stats" id="stats">
    <div class="stat"><div class="stat-val" id="s-total">—</div><div class="stat-lbl">TOTAL DATABOX</div></div>
    <div class="stat"><div class="stat-val" id="s-img">—</div><div class="stat-lbl">IMAGES</div></div>
    <div class="stat"><div class="stat-val" id="s-txt">—</div><div class="stat-lbl">TEXT</div></div>
    <div class="stat"><div class="stat-val" id="s-prg">—</div><div class="stat-lbl">PROGRAMS</div></div>
  </div>

  <!-- DataBox List -->
  <div class="panel">
    <div class="panel-hdr">
      <h2>▸ DATABOX INDEX</h2>
      <button class="btn-submit" onclick="loadList()" style="padding:5px 14px;font-size:11px">⟳ REFRESH</button>
    </div>
    <div class="panel-body">
      <table>
        <thead>
          <tr>
            <th>ID</th><th>TYPE</th><th>SIZE</th><th>PRICE</th><th>RANK</th><th>ACTION</th>
          </tr>
        </thead>
        <tbody id="db-list">
          <tr><td colspan="6" class="empty">Loading...</td></tr>
        </tbody>
      </table>
    </div>
  </div>

  <!-- Upload -->
  <div class="panel">
    <div class="panel-hdr"><h2>▸ UPLOAD NEW DATABOX</h2></div>
    <div class="panel-body">
      <div class="upload-form">
        <div>
          <label class="lbl">Type</label>
          <select id="u-type">
            <option value="TXT">TXT — Text</option>
            <option value="IMG">IMG — Image</option>
            <option value="CFG">CFG — Config</option>
            <option value="PRG">PRG — Program</option>
            <option value="VID">VID — Video</option>
          </select>
        </div>
        <div>
          <label class="lbl">Price (Token)</label>
          <input type="number" id="u-price" value="0" min="0">
        </div>
        <div class="full">
          <label class="lbl">Metadata (JSON)</label>
          <input type="text" id="u-meta" placeholder='{"title":"My File","desc":"..."}' value='{"title":"","desc":""}'>
        </div>
        <div class="full">
          <label class="lbl">Payload (Text)</label>
          <textarea id="u-payload" placeholder="ข้อมูลที่ต้องการเก็บ..."></textarea>
        </div>
        <div class="full">
          <button class="btn-submit" onclick="uploadDB()">[ UPLOAD DATABOX ]</button>
          <div id="msg"></div>
        </div>
      </div>
    </div>
  </div>
</main>

<script>
function loadList(){
  fetch('/api/list').then(r=>r.json()).then(data=>{
    const tbody=document.getElementById('db-list');
    if(!data.length){tbody.innerHTML='<tr><td colspan="6" class="empty">ไม่มี DataBox</td></tr>';return;}
    let html='',total=data.length,img=0,txt=0,prg=0;
    data.forEach(d=>{
      if(d.type==='IMG')img++;
      if(d.type==='TXT')txt++;
      if(d.type==='PRG')prg++;
      html+=`<tr>
        <td class="mono" style="font-size:12px">${d.id}</td>
        <td><span class="badge badge-${d.type}">${d.type}</span></td>
        <td class="mono">${fmtSize(d.size)}</td>
        <td class="mono">${d.price>0?d.price+' T':'FREE'}</td>
        <td class="mono">${d.rank}</td>
        <td><button class="del-btn" onclick="delDB('${d.id}')">DEL</button></td>
      </tr>`;
    });
    tbody.innerHTML=html;
    document.getElementById('s-total').textContent=total;
    document.getElementById('s-img').textContent=img;
    document.getElementById('s-txt').textContent=txt;
    document.getElementById('s-prg').textContent=prg;
  }).catch(()=>{
    document.getElementById('db-list').innerHTML='<tr><td colspan="6" class="empty">ไม่สามารถโหลดได้</td></tr>';
  });
}
function fmtSize(b){
  if(b<1024)return b+'B';
  if(b<1048576)return (b/1024).toFixed(1)+'KB';
  return (b/1048576).toFixed(1)+'MB';
}
function delDB(id){
  if(!confirm('ลบ '+id+' ?'))return;
  const fd=new FormData();fd.append('id',id);
  fetch('/api/delete',{method:'POST',body:fd})
    .then(r=>r.json()).then(d=>{if(d.ok)loadList();});
}
function uploadDB(){
  const msg=document.getElementById('msg');
  msg.style.color='var(--accent)';msg.textContent='กำลังอัปโหลด...';
  const fd=new FormData();
  fd.append('type',document.getElementById('u-type').value);
  fd.append('price',document.getElementById('u-price').value);
  fd.append('meta',document.getElementById('u-meta').value);
  fd.append('payload',document.getElementById('u-payload').value);
  fetch('/api/upload',{method:'POST',body:fd})
    .then(r=>r.json()).then(d=>{
      if(d.ok){
        msg.style.color='var(--accent3)';
        msg.textContent='✓ Upload สำเร็จ';
        loadList();
      } else {
        msg.style.color='var(--accent2)';
        msg.textContent='✗ Upload ล้มเหลว: '+(d.error||'');
      }
    }).catch(()=>{msg.style.color='var(--accent2)';msg.textContent='✗ Connection error';});
}
loadList();
</script>)";
    h += _htmlFoot();
    return h;
}
