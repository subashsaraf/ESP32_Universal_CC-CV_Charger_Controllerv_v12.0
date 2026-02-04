//************************************************************************************
// charger_web.cpp
//
// Web UI + JSON API implementation for ESP32 Universal CC/CV Charger Controller.
//
// - Implements all logic declared in charger_web.h
// - Registers HTTP routes and handlers
// - Builds HTML dashboard, WiFi page, and JSON API
// - Handles load control, WiFi config, security, and uptime
//
// Interface: charger_web.h
//
//************************************************************************************

#include "charger_web.h"
#include "charger_web_routes.h"
#include <WiFi.h>
#include <WebServer.h>
#include "fan_control.h"

//*****************************************************************************
// Global system state tracking variables (implementation)
//*****************************************************************************
uint32_t bootTimeMs = 0;              // System boot time in milliseconds
String wifiSSID = "";                 // Current WiFi SSID cache
String localIP = "";                  // Current local IP cache
uint32_t lastNetworkUpdate = 0;       // Last network info update timestamp

//************************************************************************************
// Mode Configuration Structure and Helper Functions
//************************************************************************************

// Mode configuration structure - SINGLE SOURCE OF TRUTH for mode colors
struct ModeConfig {
  const char* modeName;
  const char* displayName;
  const char* description;
  const char* color;        // Only color source for this mode
  const char* bgColor;
};

// Mode configurations - SINGLE SOURCE OF TRUTH for all modes and sub-states
const ModeConfig modeConfigs[] = {
  {"BULK",       "BULK",       "Fast Charging",    "#ef4444", "rgba(239,68,68,0.25)"},    // Red - Pulsing red glow for active/fast charging
  {"ABSORPTION", "ABSORPTION", "CV Charging",      "#f59e0b", "rgba(245,158,11,0.25)"},   // Orange - Glowing amber for constant voltage
  {"FLOAT",      "FLOAT",      "Fully Charged",    "#22c55e", "rgba(34,197,94,0.25)"},    // Green - Soft green pulse for maintenance
  {"BACKUP",     "BACKUP",     "Battery Backup",   "#8b5cf6", "rgba(139,92,246,0.25)"},   // Purple - Pulsing purple for backup mode
  {"BACKUP_BATT", "BACKUP",     "Battery Powering Load", "#ec4899", "rgba(236,72,153,0.25)"}, // Pink - Flashing pink for active discharge
  {"BACKUP_IDLE", "BACKUP",     "Battery Idle",    "#3b82f6", "rgba(59,130,246,0.25)"},   // Blue - Breathing blue for idle backup
  {"IDLE",       "IDLE",       "Ready To Work",    "#fde047", "rgba(253,224,71,0.25)"}    // Fluorescent Yellow - Bright pulsating yellow
};

const int MODE_COUNT = sizeof(modeConfigs) / sizeof(modeConfigs[0]);

// Helper function to parse mode string from LCD format
String parseModeForWeb(const String& modeStr) {
  String modeClean = modeStr;
  modeClean.trim();

  // Remove parentheses from modes (like "(BULK)", "(ABSO)", etc.)
  if (modeClean.startsWith("(") && modeClean.endsWith(")")) {
    modeClean = modeClean.substring(1, modeClean.length() - 1);
  }

  // Extract just the mode name (first word before any space)
  int spaceIndex = modeClean.indexOf(' ');
  if (spaceIndex > 0) {
    modeClean = modeClean.substring(0, spaceIndex);
  }

  // Convert common LCD abbreviations to full mode names
  if (modeClean.equalsIgnoreCase("ABSO")) return "ABSORPTION";
  if (modeClean.equalsIgnoreCase("FLOT")) return "FLOAT";
  if (modeClean.equalsIgnoreCase("BATT")) return "BACKUP";
  if (modeClean.equalsIgnoreCase("B-IDL")) return "BACKUP";

  return modeClean;
}

// Get mode configuration based on mode string
const ModeConfig* getModeConfig(const String& mode) {
  // Check for empty or null mode
  if (mode.length() == 0) {
    return &modeConfigs[6]; // Default to IDLE (index 6)
  }

  // Special handling for BACKUP sub-states (check original mode string directly)
  if (mode == "(BATT)") {
    return &modeConfigs[4]; // BACKUP_BATT - Battery powering load
  }
  if (mode == "(B-IDL)") {
    return &modeConfigs[5]; // BACKUP_IDLE - Battery idle
  }

  // Parse for other modes
  String modeClean = parseModeForWeb(mode);
  modeClean.toUpperCase();

  // Check for exact match first
  for (int i = 0; i < MODE_COUNT; i++) {
    if (modeClean.equalsIgnoreCase(modeConfigs[i].modeName)) {
      return &modeConfigs[i];
    }
  }

  // Check for partial matches (fallback)
  if (modeClean.indexOf("BULK") >= 0 || modeClean.indexOf("BUCK") >= 0) {
    return &modeConfigs[0]; // BULK
  }
  if (modeClean.indexOf("ABS") >= 0 || modeClean.indexOf("ABSO") >= 0) {
    return &modeConfigs[1]; // ABSORPTION
  }
  if (modeClean.indexOf("FLOAT") >= 0 || modeClean.indexOf("FLO") >= 0) {
    return &modeConfigs[2]; // FLOAT
  }
  if (modeClean.indexOf("BACKUP") >= 0 || modeClean.indexOf("BACK") >= 0) {
    return &modeConfigs[3]; // BACKUP (generic)
  }

  // Default to IDLE if mode not found
  return &modeConfigs[6]; // IDLE
}

//************************************************************************************
// System Time Management
//************************************************************************************

void initUptime() {
  bootTimeMs = millis();
}

int getUptimeSeconds() {
  return (millis() - bootTimeMs) / 1000;
}

String formatUptimeString() {
  int sec = getUptimeSeconds();
  int days = sec / 86400;
  int hours = (sec % 86400) / 3600;
  int minutes = (sec % 3600) / 60;
  int seconds = sec % 60;

  String s;
  s.reserve(16);

  if (days > 0) s += String(days) + "d ";
  if (hours > 0) s += String(hours) + "h ";
  if (minutes > 0) s += String(minutes) + "m ";
  s += String(seconds) + "s";

  s.trim();
  return s;
}

//*****************************************************************************
// Utility Functions for UI/API Builders
//*****************************************************************************

String getWiFiBars(int q) {
  int bars = (q * 5) / 100;                // map to 0..5
  if (bars < 0) bars = 0;
  if (bars > 5) bars = 5;

  String s;
  s.reserve(5);
  for (int i = 0; i < 5; i++)
    s += (i < bars) ? "|" : ".";
  return s;
}

String formatCurrentStr() {
  const float input_voltage_min = 2.0;  // Minimum voltage to consider input active
  String suffix = "";

  // Check if we're in BACKUP mode
  String modeClean = mode_str;
  modeClean.trim();

  // If mode is "(BATT)" or "(B-IDL)", we're in BACKUP mode
  if (modeClean == "(BATT)" || modeClean == "(B-IDL)") {
    if (load_is_on) {
      suffix = " (BATT)";     // Battery actively powering load
    } else {
      suffix = " (B-IDL)";   // Battery idle (no load or tripped)
    }
  }
  // Otherwise use normal logic
  else if (input_voltage <= input_voltage_min || input_voltage < 0.1) {
    suffix = " (BAT)";        // No significant input - battery current
  } else {
    suffix = " (POW)";        // Input voltage present - input current
  }

  // Format current value with appropriate units and source indicator
  if (abs(Current_A) >= 1.0f) {
    return String(abs(Current_A), 2) + " A" + suffix;
  }
  return String(abs(Current_A) * 1000.0f, 0) + " mA" + suffix;
}

int dutyPercentCapAware() {
  const int maxDuty = (mode_str == BULK_MODE_LABEL) ? 230 : 255;
  if (maxDuty <= 0) return 0;                  // defensive guard

  // Ensure pwm_value is within valid range before division
  const int safe_pwm = (pwm_value < 0) ? 0 : (pwm_value > maxDuty) ? maxDuty : pwm_value;
  int pct = (safe_pwm * 100) / maxDuty;        // convert to percent
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

String htmlEscape(const String& s) {
  // Reserve worst-case scenario: all characters need escaping (6 bytes per char)
  String out;
  out.reserve(s.length() * 6);

  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    switch (c) {
      case '&': out += F("&amp;");  break;
      case '<': out += F("&lt;");   break;
      case '>': out += F("&gt;");   break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += c;
    }
  }
  return out;
}

String jsonEscape(const String& s) {
  // Reserve worst-case scenario: all characters need escaping
  String out;
  out.reserve(s.length() * 6);

  for (size_t i = 0; i < s.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    switch (c) {
      case '\"': out += F("\\\""); break;
      case '\\': out += F("\\\\"); break;
      case '\b': out += F("\\b");  break;
      case '\f': out += F("\\f");  break;
      case '\n': out += F("\\n");  break;
      case '\r': out += F("\\r");  break;
      case '\t': out += F("\\t");  break;
      default:
        if (c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04X", c);
          out += buf;
        } else {
          out += char(c);
        }
    }
  }
  return out;
}

String encToStr(uint8_t auth) {
  switch (auth) {
    case 0:  return "OPEN";
    case 1:  return "WEP";
    case 2:  return "WPA";
    case 3:  return "WPA2";
    case 4:  return "WPA/WPA2";
    case 5:  return "WPA2-ENT";
    case 6:  return "WPA3";
    case 7:  return "WPA2/WPA3";
    case 8:  return "WAPI";
    case 9:  return "OWE";
    default: return "UNKNOWN";
  }
}

void applyLoad(bool on) {
  load_is_on = on;                                 // in-memory flag
  load_status = on ? "On " : "Off";                // legacy textual label
  const bool pinLevel = LOAD_ACTIVE_HIGH ? on : !on;
  digitalWrite(load_enable, pinLevel);             // drive output pin last
}

String formatLoadText(bool isOn, bool isManual) {
  bool showOn = isOn, showManual = isManual;
#if INVERT_LOAD_LABELS
  showOn = !showOn;
  showManual = !showManual;
#endif

  String s;
  s.reserve(16);
  s += (showOn ? "ON" : "OFF");
  s += " • ";
  s += (showManual ? "MANUAL" : "AUTO");
  return s;
}

//************************************************************************************
// Battery Status Functions
//************************************************************************************

String getBatteryStatusText(int soc) {
  if (soc <= 20) return String(F("{Crit}"));
  if (soc <= 50) return String(F("{Warn}"));
  if (soc <= 94) return String(F("{Good}"));
  return String(F("{Full}"));
}

const char* getBatteryStatusCode(int soc) {
  if (soc <= 20) return "crit";
  if (soc <= 50) return "warn";
  if (soc <= 94) return "good";
  return "full";
}

//*****************************************************************************
// Security Functions
//*****************************************************************************

void addSecurityHeaders() {
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.sendHeader("Referrer-Policy", "no-referrer");
  server.sendHeader("X-Frame-Options", "DENY");
  server.sendHeader("X-DNS-Prefetch-Control", "off");
  server.sendHeader("Permissions-Policy", "interest-cohort=()");
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");

  // Additional security headers for modern browsers
  server.sendHeader("Cross-Origin-Opener-Policy", "same-origin");
  server.sendHeader("Cross-Origin-Embedder-Policy", "require-corp");

  // Content-Security-Policy:
  // - 'unsafe-inline' is present because the current HTML includes inline <script>.
  // - To tighten CSP remove 'unsafe-inline' and move scripts/styles to external endpoints
  //   or implement a per-page nonce (recommended for production).
  String csp;
  csp.reserve(200);
  csp += "default-src 'self'; frame-ancestors 'none'; connect-src 'self' http://";
  csp += String(MDNS_HOST);
  csp += ".local; img-src 'self' data:; style-src 'self' 'unsafe-inline';";
  csp += " script-src 'self' 'unsafe-inline'";
  server.sendHeader("Content-Security-Policy", csp);
}

String hostOnlyFromURL(const String& url) {
  int p = url.indexOf(F("://"));
  if (p < 0) return String();

  p += 3;
  int e = url.indexOf('/', p);
  if (e < 0) e = url.length();

  String host = url.substring(p, e);
  int colon = host.indexOf(':');
  if (colon > 0) host.remove(colon);
  host.toLowerCase();

  return host;
}

String stripPortLower(const String& hostHeader) {
  String h = hostHeader;
  h.toLowerCase();
  int colon = h.indexOf(':');
  if (colon > 0) h.remove(colon);
  return h;
}

bool csrfOK() {
  HTTPMethod m = server.method();
  if (m == HTTP_GET || m == HTTP_HEAD || m == HTTP_OPTIONS) return true;

  // If no Origin header then many CLI tools will succeed; optionally check Referer below.
  if (!server.hasHeader(F("Origin"))) {
    // If a Referer is present we can perform the same-origin check using it.
    if (server.hasHeader(F("Referer"))) {
      String referer = server.header(F("Referer"));
      String originHost = hostOnlyFromURL(referer);
      if (originHost.length() == 0) return false;

      String host = stripPortLower(server.hostHeader());
      String ip   = WiFi.localIP().toString();
      ip.toLowerCase();
      String mdns = String(MDNS_HOST);
      mdns += ".local";

      if (host.length() && originHost == host) return true;
      if (originHost == ip) return true;
      if (originHost == mdns) return true;
      return false;
    }

#if CSRF_ALLOW_NULL_ORIGIN
    // Allow missing Origin (legacy/tools) if opted-in via compile-time flag.
    return true;
#else
    return false;
#endif
  }

  // Validate Origin header explicitly.
  String origin = server.header(F("Origin"));
  origin.trim();

#if CSRF_ALLOW_NULL_ORIGIN
  String ol = origin;
  ol.toLowerCase();
  if (ol == "null") return true;                  // allow file:// POSTs if enabled
#endif

  String originHost = hostOnlyFromURL(origin);
  if (originHost.length() == 0) return false;

  String host = stripPortLower(server.hostHeader());
  String ip   = WiFi.localIP().toString();
  ip.toLowerCase();
  String mdns = String(MDNS_HOST);
  mdns += ".local";

  if (host.length() && originHost == host) return true;
  if (originHost == ip) return true;
  if (originHost == mdns) return true;

  return false;
}

//*****************************************************************************
// Network Information Management
//*****************************************************************************

void updateNetworkInfo() {
  const uint32_t now = millis();
  // Update every 5 seconds or if never updated (safe millis wrap-around check)
  if ((lastNetworkUpdate == 0) || ((now - lastNetworkUpdate) > 5000)) {
    wifiSSID = WiFi.SSID();
    localIP = WiFi.localIP().toString();
    lastNetworkUpdate = now;
  }
}

//************************************************************************************
// WiFi Configuration Page Builder
//************************************************************************************

String makeWiFiHTML() {
  // Update cached network info
  updateNetworkInfo();

  const String ssidSafe = htmlEscape(wifiSSID);
  const String ipStr    = localIP;
  const String mdnsURL  = "http://" + String(MDNS_HOST) + ".local";

  String html;
  html.reserve(7200);

  // Top-level HTML and CSS (dark theme)
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<meta name='color-scheme' content='dark'>");
  html += F("<title>WiFi Setup</title>");
  html += F("<style>");
  html += F(":root{--bg:#0d1117;--panel:#0f1623;--line:#1f2a3b;--text:#e6edf3;");
  html += F("--muted:#9aa3b2;--primary:#3b82f6;--info:#06b6d4}");
  html += F("*{box-sizing:border-box}html,body{height:100%;margin:0}");
  html += F("body{background:var(--bg);color:var(--text);");
  html += F("font:14px/1.35 -apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Arial,sans-serif}");
  html += F(".wrap{min-height:100svh;display:grid;place-items:center;padding:16px}");
  html += F(".card{background:var(--panel);border:1px solid var(--line);");
  html += F("border-radius:12px;max-width:560px;width:100%;padding:16px}");
  html += F(".title{font-weight:800;font-size:18px;margin-bottom:6px}");
  html += F(".muted{color:var(--muted);font-size:12px;margin-bottom:10px}");
  html += F("label{font-size:12px;color:var(--muted);display:block;margin:10px 0 4px}");
  html += F(".input{width:100%;padding:10px 12px;border-radius:8px;");
  html += F("border:1px solid var(--line);background:#0c1421;color:var(--text)}");
  html += F(".btnrow{display:flex;gap:8px;flex-wrap:wrap;justify-content:center;margin-top:14px}");
  html += F(".chipbtn{appearance:none;border:1px solid var(--line);");
  html += F("background:#0c1421;color:#e7e9ee;border-radius:999px;");
  html += F("padding:8px 14px;font-weight:800;cursor:pointer;font-size:13px;min-height:38px}");
  html += F(".chipbtn.primary{background:var(--primary);border-color:var(--primary);color:#fff}");
  html += F(".chipbtn.info{background:var(--info);border-color:var(--info);color:#06202a}");
  html += F(".status{font-size:12px;color:#c7d0dd;margin-top:8px;text-align:center}");
  html += F(".overlay{position:fixed;inset:0;background:rgba(10,14,20,.85);");
  html += F("display:none;align-items:center;justify-content:center;z-index:9999}");
  html += F(".obox{background:#0f1623;border:1px solid var(--line);");
  html += F("border-radius:12px;padding:16px 18px;text-align:center;max-width:360px;color:#e6edf3}");
  html += F(".spin{width:28px;height:28px;border:3px solid #2a2f3a;");
  html += F("border-top-color:#60a5fa;border-radius:50%;margin:10px auto 8px;");
  html += F("animation:sp 1s linear infinite}");
  html += F("@keyframes sp{to{transform:rotate(360deg)}}");
  html += F("</style></head><body><div class='wrap'>");

  // Card: title + current SSID/IP
  html += "<div class='card'>";
  html += "<div class='title'>WiFi Setup</div>";
  html += "<div class='muted'>Current SSID: " + ssidSafe + " • IP: " + ipStr + "</div>";

  // Scanner UI (button + select) and status line
  html += F("<label>Available Networks</label>");
  html += F("<div class='btnrow'><button type='button' class='chipbtn info' id='btnScan'>");
  html += F("Scan Networks</button></div>");
  html += F("<select class='input' id='networks' size='8'>");
  html += F("<option disabled selected>Click \"Scan Networks\"...</option></select>");
  html += F("<div class='status' id='scanStatus'></div>");

  // WiFi form (AJAX POST to /api/wifi_save preferred)
  html += F("<form id='wifiForm' method='POST' action='/api/wifi_save'>");
  html += F("<label for='ssid'>SSID</label>");
  html += F("<input class='input' id='ssid' name='ssid' type='text' placeholder='Your WiFi SSID' required>");
  html += F("<label for='pass'>Password</label>");
  html += F("<input class='input' id='pass' name='pass' type='password' placeholder='WiFi password (if any)'>");
  html += F("<div class='btnrow'>");
  html += F("<button type='submit' class='chipbtn primary'>Save & Reboot</button>");
  html += F("<a class='chipbtn' href='/'>&larr; Back</a>");
  html += F("</div>");
  html += F("</form>");

  // Overlay shown after Save & Reboot with 120s countdown
  html += F("<div class='overlay' id='ov'>");
  html += F("<div class='obox'>");
  html += F("<div style='font-weight:800;margin-bottom:6px'>Saving WiFi…</div>");
  html += F("<div class='spin'></div>");
  html += F("<div class='status'>Saved SSID: <strong id='savedSsid'></strong></div>");
  html += "<div class='status'>IP Address: <strong id='savedIp'>" + ipStr + "</strong></div>";
  html += F("<div class='status'>Returning to Home in <span id='cd'>120</span>s or sooner if connected.</div>");
  html += F("</div>");
  html += F("</div>");

  // Client-side JS embedded: scanning, form submit, countdown, mDNS/local probe
  html += F("<script>const MDNS_URL='");
  html += mdnsURL + F("';");
  html += F("const $=id=>document.getElementById(id);");
  html += F("const netsEl=$('networks'), ssidEl=$('ssid'), st=$('scanStatus'), btnScan=$('btnScan');");
  html += F("const form=$('wifiForm'), ov=$('ov'), saved=$('savedSsid');");
  html += F("function bars(r){");
  html += F("return r>=-55?'|||||':r>=-67?'||||':r>=-80?'|||':r>=-90?'||':'|';}");
  html += F("async function scan(){");
  html += F("try{st.textContent='Scanning...'; netsEl.innerHTML='';");
  html += F("const r=await fetch('/api/scan',{cache:'no-store',");
  html += F("headers:{'X-RequestedWith':'XMLHttpRequest'}}); if(!r.ok) throw 0;");
  html += F("const j=await r.json();");
  html += F("const list=(j&&j.nets)||[];");
  html += F("if(!list.length){netsEl.innerHTML='<option disabled>No networks found</option>';");
  html += F("st.textContent='No networks found'; return;}");
  html += F("list.sort((a,b)=>b.rssi-a.rssi);");
  html += F("for(const n of list){");
  html += F("const o=document.createElement('option');");
  html += F("o.value=n.ssid; o.textContent=n.ssid+'  '+(n.enc!=='OPEN'?'🔒 ':'')+");
  html += F("bars(n.rssi)+'  ('+n.rssi+' dBm, ch '+n.ch+')';");
  html += F("netsEl.appendChild(o);}");
  html += F("st.textContent=list.length+' network(s)';");
  html += F("}catch(e){netsEl.innerHTML='<option disabled>Scan failed</option>';");
  html += F("st.textContent='Scan failed';}}");
  html += F("if(btnScan) btnScan.onclick=scan;");
  html += F("if(netsEl) netsEl.onchange=()=>{ if(netsEl.value) ssidEl.value=netsEl.value; };");
  html += F("document.addEventListener('DOMContentLoaded', ()=>{ if(btnScan) btnScan.click(); });");
  html += F("function sleep(ms){return new Promise(r=>setTimeout(r,ms));}");
  html += F("async function pingEither(ms=120000,step=1000){");
  html += F("const start=Date.now();");
  html += F("while(Date.now()-start<ms){");
  html += F("try{const r=await fetch('/api',{cache:'no-store',");
  html += F("headers:{'X-RequestedWith':'XMLHttpRequest'}}); if(r.ok) return 'local';}catch(e){}");
  html += F("try{if('AbortController' in window){");
  html += F("const c=new AbortController(); const to=setTimeout(()=>c.abort(),800);");
  html += F("await fetch(MDNS_URL+'/api',{cache:'no-store',mode:'no-cors',signal:c.signal});");
  html += F("clearTimeout(to); return 'mdns';}else{");
  html += F("await fetch(MDNS_URL+'/api',{cache:'no-store',mode:'no-cors'}); return 'mdns';}");
  html += F("}catch(e){}");
  html += F("await sleep(step);}");
  html += F("return false;}");
  html += F("if(form) form.addEventListener('submit', async (ev)=>{");
  html += F("ev.preventDefault();");
  html += F("const ssid=(ssidEl&&ssidEl.value||'').trim();");
  html += F("if(!ssid){ alert('SSID is required'); return; }");
  html += F("if(!confirm('Save WiFi and reboot now?')) return;");
  html += F("if(saved) saved.textContent=ssid;");
  html += F("if(ov) ov.style.display='flex';");
  html += F("try{const fd=new FormData(form);");
  html += F("await fetch('/api/wifi_save',{method:'POST',body:fd,");
  html += F("headers:{'Accept':'application/json','X-RequestedWith':'XMLHttpRequest'}});");
  html += F("}catch(e){}");
  html += F("let sec=120; const timer=setInterval(()=>{sec--;");
  html += F("const cdElement = document.getElementById('cd');");
  html += F("if(cdElement) cdElement.textContent=sec;");
  html += F("if(sec<=0){clearInterval(timer); location.replace('/');}},1000);");
  html += F("const res=await pingEither(120000,1000);");
  html += F("if(res){ clearInterval(timer); location.replace(res==='mdns'?MDNS_URL+'/':'/'); }");
  html += F("});</script>");

  html += "</div></div></body></html>";
  return html;
}

//*****************************************************************************
// Dashboard Page Builder - WITH GLOWING MODE EFFECTS
//*****************************************************************************

String makeHTML() {
  // Update cached network info
  updateNetworkInfo();

  // Prepare initial seed values used by the first paint; JS will update as /api results arrive.
  const int wifiQ = constrain(getWiFiSignalQuality(), 0, 100);
  const int dutyPct = dutyPercentCapAware();
  const int soc = constrain(getBatteryPercentage(bat_voltage), 0, 100);
  const String currStr = formatCurrentStr();
  const bool isManual = (loadControlModeStr == "M");
  const bool isOn = load_is_on;
  const int fanPct = (FAN_MAX_DUTY > 0) ?
                     constrain((getFanDuty() * 100) / FAN_MAX_DUTY, 0, 100) : 0;

  // Battery status (display and machine code)
  const String batStatusText = getBatteryStatusText(soc);
  const char* batStatusCode = getBatteryStatusCode(soc);

  // Uptime (formatted as "1d 3h 45m 12s")
  const int uptimeSec = getUptimeSeconds();
  const String uptimeStr = formatUptimeString();

  // Get mode configuration directly from original mode_str
  const ModeConfig* currentModeConfig = getModeConfig(mode_str);

  // Use values from config structure
  String modeDisplay = String(currentModeConfig->displayName);
  String modeDescription = String(currentModeConfig->description);
  String modeColor = String(currentModeConfig->color);

  // Determine glow class for current mode
  String glowClass = "idle-glow"; // Default
  if (modeDisplay == "BULK") glowClass = "bulk-glow";
  else if (modeDisplay == "ABSORPTION") glowClass = "absorption-glow";
  else if (modeDisplay == "FLOAT") glowClass = "float-glow";
  else if (modeDisplay == "BACKUP") {
    if (mode_str == "(BATT)") glowClass = "backup-batt-glow";
    else if (mode_str == "(B-IDL)") glowClass = "backup-idle-glow";
    else glowClass = "backup-glow";
  }

  // Safe strings for HTML rendering
  const String modeSafe = htmlEscape(modeDisplay);
  const String modeDescSafe = htmlEscape(modeDescription);
  const String ssidSafe = htmlEscape(wifiSSID);
  const String batteryNameSafe = htmlEscape(String(currentBattery.name));
  const String loadText = formatLoadText(isOn, isManual);
  const String ipStr = localIP;

  const String mdnsURL  = "http://" + String(MDNS_HOST) + ".local";
  const String mdnsHost = String(MDNS_HOST) + ".local";

  String html;
  html.reserve(HTML_RESERVE_SIZE);

  // Head, CSS, and initial layout
  html += F("<!DOCTYPE html><html><head><meta charset='utf-8'>");
  html += F("<meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<meta name='theme-color' content='#0d1117'>");
  html += F("<meta name='color-scheme' content='dark'>");
  html += F("<title>ESP32 CC/CV Charger Controller</title>");

  // CSS Styles
  html += F("<style>");
  html += F(":root{--bg:#0d1117;--panel:#0f1623;--line:#1f2a3b;--text:#e6edf3;--muted:#9aa3b2;");
  html += F("--primary:#3b82f6;--secondary:#a855f7;--info:#06b6d4;--success:#22c55e;");
  html += F("--warning:#f59e0b;--danger:#ef4444;--restart-a:#f43f5e;--restart-b:#f97316;");
  html += F("--wificfg-a:#22d3ee;--wificfg-b:#6366f1}");
  html += F("*{box-sizing:border-box}html,body{height:100%;margin:0}");
  html += F("body{background:var(--bg);color:var(--text);");
  html += F("font:14px/1.35 -apple-system,BlinkMacSystemFont,Segoe UI,Roboto,Arial,sans-serif;");
  html += F("-webkit-font-smoothing:antialiased;font-variant-numeric:tabular-nums}");
  html += F(".wrap{min-height:100svh;display:grid;grid-template-rows:auto 1fr auto;");
  html += F("gap:8px;max-width:860px;margin:0 auto;padding:10px}");
  html += F(".header{display:flex;align-items:center;justify-content:center;padding:6px 0}");
  html += F(".hero{display:flex;flex-direction:column;align-items:center;text-align:center;gap:4px}");
  html += F(".title{font-weight:900;font-size:clamp(20px,4.6vw,26px);");
  html += F("letter-spacing:.2px;text-transform:uppercase;margin:0}");
  html += F(".subtitle{font-weight:800;font-size:12px;letter-spacing:.16em;");
  html += F("text-transform:uppercase;opacity:.95;margin:0}");
  html += F(".byline{font-size:12px;color:#c7d0dd;letter-spacing:.06em}");
  html += F(".rainbow{background:linear-gradient(90deg,#60a5fa,#34d399,#f59e0b,#ef4444,#8b5cf6);");
  html += F("background-clip:text;color:transparent}");
  html += F(".card{background:var(--panel);border:1px solid var(--line);");
  html += F("border-radius:12px;padding:10px}");
  html += F(".metrics{display:grid;gap:8px;");
  html += F("grid-template-columns:repeat(auto-fit,minmax(150px,1fr))}");
  html += F(".metric{background:#0c1421;border:1px solid var(--line);");
  html += F("border-radius:10px;padding:10px;text-align:center}");
  html += F(".k{font-size:11px;color:var(--muted);letter-spacing:.3px;");
  html += F("text-transform:uppercase}");
  html += F(".v{font-weight:800;font-size:18px;margin-top:2px}");
  html += F(".bars,.states,.controls{display:grid;gap:8px;margin-top:8px;");
  html += F("grid-template-columns:repeat(2,1fr)}");
  html += F("@media(max-width:620px){.bars,.states,.controls{grid-template-columns:1fr}}");
  html += F(".barwrap{background:#0c1421;border:1px solid var(--line);");
  html += F("border-radius:10px;padding:10px;text-align:center}");
  html += F(".bar{height:10px;border-radius:999px;background:#111b2b;");
  html += F("border:1px solid var(--line);overflow:hidden;position:relative}");
  html += F(".fill{height:100%;");
  html += F("background:linear-gradient(90deg,#4f8cff,#22c55e 65%,#f59e0b);");
  html += F("transition:width 0s ease}");
  html += F(".state{background:#0c1421;border:1px solid var(--line);");
  html += F("border-radius:10px;padding:10px;text-align:center}");

  // Button styles
  html += F(".btnrow{display:flex;align-items:stretch;justify-content:center;");
  html += F("gap:10px;flex-wrap:wrap}");
  html += F(".chipbtn{appearance:none;border:1px solid var(--line);");
  html += F("background:#0c1421;color:#e7e9ee;border-radius:999px;");
  html += F("padding:10px 16px;font-weight:800;cursor:pointer;");
  html += F("font-size:clamp(14px,2.6vw,17px);min-height:44px;line-height:1.2;");
  html += F("flex:1 1 160px;text-transform:uppercase;");
  html += F("transition:filter .1s ease,transform .03s ease,box-shadow .1s ease}");
  html += F(".chipbtn:hover{filter:brightness(1.08)}");
  html += F(".chipbtn:active{transform:translateY(1px)}");
  html += F(".chipbtn.toggle:hover{box-shadow:0 0 0 2px rgba(59,130,246,.24) inset}");
  html += F(".chipbtn.primary{background:var(--primary);border-color:var(--primary);color:#fff}");
  html += F(".chipbtn.secondary{background:var(--secondary);border-color:var(--secondary);color:#fff}");
  html += F(".chipbtn.ok{background:var(--success);border-color:var(--success);color:#0b1115}");
  html += F(".chipbtn.danger{background:var(--danger);border-color:var(--danger);color:#fff}");
  html += F(".chipbtn.restart{background:linear-gradient(90deg,var(--restart-a),var(--restart-b));");
  html += F("border-color:var(--restart-a);color:#fff;background-size:200% 100%}");
  html += F(".chipbtn.wificfg{background:linear-gradient(90deg,var(--wificfg-a),var(--wificfg-b));");
  html += F("border-color:#3b82f6;color:#fff;background-size:200% 100%}");

  // Status bar
  html += F(".statusbar{display:flex;gap:10px;flex-wrap:wrap;justify-content:center;");
  html += F("align-items:center;margin:6px auto 0;max-width:860px}");
  html += F(".sitem{display:flex;align-items:center;gap:6px;background:#0c1421;");
  html += F("border:1px solid var(--line);border-radius:999px;padding:6px 10px}");
  html += F(".sv{font-weight:700;color:#e6edf3}");

  // WiFi bar
  html += F(".wbar{width:120px;height:8px;border-radius:999px;background:#111b2b;");
  html += F("border:1px solid var(--line);overflow:hidden}");
  html += F(".wfill{height:100%;");
  html += F("background:linear-gradient(90deg,#ef4444 0%,#f59e0b 45%,#22c55e 100%)}");
  html += F("@media(max-width:480px){.wbar{width:90px}}");

  // Overlay
  html += F(".overlay{position:fixed;inset:0;background:rgba(10,14,20,.85);");
  html += F("display:none;align-items:center;justify-content:center;z-index:9999}");
  html += F(".obox{background:#0f1623;border:1px solid var(--line);");
  html += F("border-radius:12px;padding:16px 18px;text-align:center;");
  html += F("max-width:360px;color:#e6edf3}");
  html += F(".spin{width:28px;height:28px;border:3px solid #2a2f3a;");
  html += F("border-top-color:#60a5fa;border-radius:50%;margin:8px auto 6px;");
  html += F("animation:sp 0.5s linear infinite}");
  html += F("@keyframes sp{to{transform:rotate(360deg)}}");

  // Battery status badge
  html += F(".status-badge{display:inline-block;margin-left:8px;padding:4px 8px;");
  html += F("border-radius:999px;font-weight:800;font-size:17px;vertical-align:middle;");
  html += F("position:relative;z-index:20;border:1px solid transparent;");
  html += F("animation: fadeInScale 0.3s ease-out}");
  html += F(".status-badge.crit{color:#ef4444;background:transparent !important;");
  html += F("animation: criticalShake 0.5s ease-in-out infinite alternate}");
  html += F(".status-badge.warn{color:#f59e0b;background:transparent !important;");
  html += F("animation: warnFloat 1.8s ease-in-out infinite}");
  html += F(".status-badge.good{color:#22c55e;background:transparent !important;");
  html += F("animation: goodBounce 1.5s ease-in-out infinite}");
  html += F(".status-badge.full{color:#a855f7;background:transparent !important;");
  html += F("animation: fullRainbow 3s linear infinite}");

  // Battery animation keyframes
  html += F("@keyframes fadeInScale{0%{opacity:0;transform:scale(0.8)}");
  html += F("100%{opacity:1;transform:scale(1)}}");
  html += F("@keyframes criticalShake{0%{transform:translateX(0) rotate(0deg)}");
  html += F("25%{transform:translateX(-2px) rotate(-1deg)}");
  html += F("50%{transform:translateX(0) rotate(0deg)}");
  html += F("75%{transform:translateX(2px) rotate(1deg)}");
  html += F("100%{transform:translateX(0) rotate(0deg)}}");
  html += F("@keyframes warnFloat{0%{transform:translateY(0)}");
  html += F("50%{transform:translateY(-3px)}100%{transform:translateY(0)}}");
  html += F("@keyframes goodBounce{0%{transform:translateY(0)}");
  html += F("25%{transform:translateY(-4px)}50%{transform:translateY(0)}");
  html += F("75%{transform:translateY(-2px)}100%{transform:translateY(0)}}");
  html += F("@keyframes fullRainbow{0%{filter:hue-rotate(0deg)}");
  html += F("25%{filter:hue-rotate(90deg)}50%{filter:hue-rotate(180deg)}");
  html += F("75%{filter:hue-rotate(270deg)}100%{filter:hue-rotate(360deg)}}");

  // Mode display with glowing entrance effects
  html += F(".mode-container{position:relative}");
  html += F(".mode-value{font-weight:800;font-size:18px;margin-top:2px;");
  html += F("animation: fadeInScale 0.5s ease-out}");
  
  // Different glow animations for each mode type
  html += F(".mode-value.bulk-glow{animation: bulkGlow 2s ease-in-out infinite}");
  html += F(".mode-value.absorption-glow{animation: absorptionGlow 2.5s ease-in-out infinite}");
  html += F(".mode-value.float-glow{animation: floatGlow 3s ease-in-out infinite}");
  html += F(".mode-value.backup-glow{animation: backupGlow 2.2s ease-in-out infinite}");
  html += F(".mode-value.backup-batt-glow{animation: backupBattGlow 1.5s ease-in-out infinite}");
  html += F(".mode-value.backup-idle-glow{animation: backupIdleGlow 2.8s ease-in-out infinite}");
  html += F(".mode-value.idle-glow{animation: idleGlow 2s ease-in-out infinite}");
  
  html += F(".mode-description{font-size:13px;margin-top:4px;font-weight:600;");
  html += F("animation: multiColorPulse 2s ease-in-out infinite}");

  // Glow animation keyframes for each mode
  html += F("@keyframes bulkGlow{");
  html += F("0%{text-shadow:0 0 5px rgba(239,68,68,0.5),0 0 10px rgba(239,68,68,0.3);}");
  html += F("50%{text-shadow:0 0 15px rgba(239,68,68,0.8),0 0 25px rgba(239,68,68,0.5),0 0 35px rgba(239,68,68,0.3);}");
  html += F("100%{text-shadow:0 0 5px rgba(239,68,68,0.5),0 0 10px rgba(239,68,68,0.3);}}");
  
  html += F("@keyframes absorptionGlow{");
  html += F("0%{text-shadow:0 0 5px rgba(245,158,11,0.5);}");
  html += F("50%{text-shadow:0 0 15px rgba(245,158,11,0.8),0 0 25px rgba(245,158,11,0.5);}");
  html += F("100%{text-shadow:0 0 5px rgba(245,158,11,0.5);}}");
  
  html += F("@keyframes floatGlow{");
  html += F("0%{text-shadow:0 0 5px rgba(34,197,94,0.5);}");
  html += F("50%{text-shadow:0 0 10px rgba(34,197,94,0.8),0 0 20px rgba(34,197,94,0.4);}");
  html += F("100%{text-shadow:0 0 5px rgba(34,197,94,0.5);}}");
  
  html += F("@keyframes backupGlow{");
  html += F("0%{text-shadow:0 0 5px rgba(139,92,246,0.5);}");
  html += F("50%{text-shadow:0 0 12px rgba(139,92,246,0.8),0 0 22px rgba(139,92,246,0.5);}");
  html += F("100%{text-shadow:0 0 5px rgba(139,92,246,0.5);}}");
  
  html += F("@keyframes backupBattGlow{");
  html += F("0%{text-shadow:0 0 5px rgba(236,72,153,0.5);}");
  html += F("25%{text-shadow:0 0 15px rgba(236,72,153,0.8),0 0 25px rgba(236,72,153,0.5);}");
  html += F("50%{text-shadow:0 0 20px rgba(236,72,153,0.9),0 0 30px rgba(236,72,153,0.6);}");
  html += F("75%{text-shadow:0 0 15px rgba(236,72,153,0.8),0 0 25px rgba(236,72,153,0.5);}");
  html += F("100%{text-shadow:0 0 5px rgba(236,72,153,0.5);}}");
  
  html += F("@keyframes backupIdleGlow{");
  html += F("0%{text-shadow:0 0 5px rgba(59,130,246,0.5);}");
  html += F("50%{text-shadow:0 0 10px rgba(59,130,246,0.7),0 0 18px rgba(59,130,246,0.4);}");
  html += F("100%{text-shadow:0 0 5px rgba(59,130,246,0.5);}}");
  
  html += F("@keyframes idleGlow{");
  html += F("0%{text-shadow:0 0 5px rgba(253,224,71,0.6),0 0 8px rgba(253,224,71,0.4);}");
  html += F("50%{text-shadow:0 0 15px rgba(253,224,71,0.9),0 0 25px rgba(253,224,71,0.6),0 0 35px rgba(253,224,71,0.3);}");
  html += F("100%{text-shadow:0 0 5px rgba(253,224,71,0.6),0 0 8px rgba(253,224,71,0.4);}}");

  // Fade in scale animation for mode entrance
  html += F("@keyframes fadeInScale{");
  html += F("0%{opacity:0;transform:scale(0.9);}");
  html += F("70%{opacity:1;transform:scale(1.05);}");
  html += F("100%{opacity:1;transform:scale(1);}}");

  // Multi-color pulsing animation for mode description
  html += F("@keyframes multiColorPulse{");
  html += F("0%{opacity:0.4;color:#60a5fa;}");  // Blue
  html += F("25%{opacity:0.8;color:#34d399;}"); // Green
  html += F("50%{opacity:1;color:#f59e0b;}");   // Orange/Yellow
  html += F("75%{opacity:0.8;color:#ef4444;}"); // Red
  html += F("100%{opacity:0.4;color:#8b5cf6;}"); // Purple
  html += F("}");

  // Reduced motion preference
  html += F("@media(prefers-reduced-motion:reduce){.fill{transition:none}");
  html += F(".spin{animation:none}.status-badge{animation:none}");
  html += F(".mode-value{animation:none}.mode-description{animation:none;opacity:1}}");
  html += F("</style></head><body><div class='wrap'>");

  // Header
  html += "<div class='header'>"
          "<div class='hero'>"
          "<div class='title rainbow'>ESP32 CC/CV Charger</div>"
          "<div class='subtitle rainbow' id='batteryType'>" + batteryNameSafe + "</div>"
          "<div class='byline'>Dev By Subash Saraf • "
          "<a href='mailto:subashsaraf@gmail.com' "
          "style='color:#9bbcff;text-decoration:none'>subashsaraf@gmail.com</a></div>"
          "</div>"
          "</div>";

  // Main card container
  html += "<div class='card'>";

  // Metrics row (Input V, Battery V, Current, Temp)
  html += "<div class='metrics'>";
  html += "<div class='metric'><div class='k'>Input</div>"
          "<div class='v' id='inputV'>" + String(input_voltage, 2) + " V</div></div>";
  html += "<div class='metric'><div class='k'>Battery</div>"
          "<div class='v' id='batV'>" + String(bat_voltage, 2) + " V</div></div>";
  html += "<div class='metric'><div class='k'>Current</div>"
          "<div class='v' id='current'>" + String(currStr) + "</div></div>";
  html += "<div class='metric'><div class='k'>Temp</div>"
          "<div class='v' id='temp'>" + String(temperatureC, 1) + " °C</div></div>";
  html += "</div>";

  // Bars (Battery %, PWM %, FAN SPEED %)
  html += "<div class='bars'>";

  // Battery %
  html += "<div class='barwrap'>"
          "<div class='k'>Battery %</div>"
          "<div class='v'><span id='soc'>" + String(soc) + "%</span>"
          " <span class='status-badge " + String(batStatusCode) + "' id='batStatus'>"
          + htmlEscape(batStatusText) + "</span></div>"
          "<div class='bar'><div class='fill' id='socFill' style='width:"
          + String(soc) + "%'></div></div>"
          "</div>";

  // PWM Output - Uses same gradient as other bars
  html += "<div class='barwrap'><div class='k'>PWM Output</div>"
          "<div class='v' id='pwmPct'>" + String(dutyPct) + " %</div>"
          "<div class='bar'><div class='fill' id='pwmFill' style='width:"
          + String(dutyPct) + "%'></div></div></div>";

  // Fan Speed
  html += "<div class='barwrap'><div class='k'>FAN SPEED</div>"
          "<div class='v' id='fanPct'>" + String(fanPct) + " %</div>"
          "<div class='bar'><div class='fill' id='fanFill' style='width:"
          + String(fanPct) + "%'></div></div></div>";
  html += "</div>";

  // States (Mode, Load)
  html += "<div class='states'>";

  // Mode with glowing effect
  html += "<div class='state mode-container'><div class='k'>Mode</div>"
          "<div class='mode-value " + glowClass + "' id='mode' style='color:" + modeColor + "'>"
          + modeSafe + "</div>"
          "<div class='mode-description' id='modeDescription'>"
          + modeDescSafe + "</div></div>";

  // Load
  html += "<div class='state'><div class='k'>Load</div>"
          "<div class='v' id='load'>" + loadText + "</div></div>";
  html += "</div>";

  // Controls (Load toggle, Mode select, System actions)
  html += "<div class='controls'>";

  // Load Control
  html += "<div class='state'><div class='k'>Load Control</div>"
          "<div class='btnrow'>"
          "<button class='chipbtn toggle " + String(isOn ? "ok" : "danger")
          + "' id='btnLoadToggle' data-state='" + String(isOn ? "on" : "off") + "'>"
          + String(isOn ? "ON" : "OFF") + "</button>"
          "</div></div>";

  // Mode Select
  html += "<div class='state'><div class='k'>Mode Select</div>"
          "<div class='btnrow'>"
          "<button class='chipbtn " + String(isManual ? "secondary" : "primary")
          + "' id='btnModeToggle' data-mode='" + String(isManual ? "M" : "A") + "'>"
          + String(isManual ? "MANUAL" : "AUTO") + "</button>"
          "</div></div>";

  // System buttons
  html += "<div class='state'><div class='k'>System</div>"
          "<div class='btnrow'>"
          "<button class='chipbtn restart' id='btnRestart'>RESTART</button>"
          "<button class='chipbtn wificfg' id='btnWiFiCfg'>WIFI CONFIG</button>"
          "</div></div>";

  html += "</div>"; // controls
  html += "</div>"; // card

  // Footer status (SSID • IP • WiFi % • UPTIME)
  html += "<div class='statusbar'>"
          "<div class='sitem'><span class='k'>SSID</span>"
          "<span class='sv' id='ssidVal'>" + ssidSafe + "</span></div>"
          "<div class='sitem'><span class='k'>IP</span>"
          "<span class='sv' id='ipVal'>" + ipStr + "</span></div>"
          "<div class='sitem'><span class='k'>WiFi</span>"
          "<span class='sv'><span id='wifiQ'>" + String(wifiQ) + "</span>%</span>"
          "<div class='wbar'><div class='wfill' id='wifiFill' style='width:"
          + String(wifiQ) + "%'></div></div></div>"
          "<div class='sitem'><span class='k'>Uptime</span>"
          "<span class='sv' id='uptimeStr'>" + uptimeStr + "</span></div>"
          "</div>";

  // Restart overlay
  html += "<div class='overlay' id='overlay'><div class='obox'>"
          "<div class='spin'></div>"
          "<div id='ovText' style='font-weight:800;margin-bottom:4px'>"
          "Restarting… trying to reconnect</div>"
          "<div style='font-size:13px;color:#9aa3b2'>SSID: "
          "<strong id='ovSsid'>" + ssidSafe + "</strong></div>"
          "<div style='font-size:13px;color:#9aa3b2'>IP: "
          "<strong id='ovIp'>" + ipStr + "</strong></div>"
          "<div style='font-size:13px;color:#9aa3b2'>"
          "Returning to Home in <span id='ovSec'>60</span>s "
          "or sooner when online.</div>"
          "</div></div>";

  // Client-side JavaScript
  html += F("<script>const MDNS_URL='");
  html += mdnsURL + F("';const MDNS_HOST='");
  html += mdnsHost + F("';");

  // JavaScript code
  html += F("const $=id=>document.getElementById(id);");
  html += F("const els={");
  html += F("inputV:$('inputV'),batV:$('batV'),current:$('current'),temp:$('temp'),");
  html += F("pwmPct:$('pwmPct'),pwmFill:$('pwmFill'),soc:$('soc'),socFill:$('socFill'),");
  html += F("batStatus:$('batStatus'),fanPct:$('fanPct'),fanFill:$('fanFill'),");
  html += F("mode:$('mode'),modeDescription:$('modeDescription'),load:$('load'),");
  html += F("wifiQ:$('wifiQ'),wifiFill:$('wifiFill'),uptimeStr:$('uptimeStr'),");
  html += F("ssidVal:$('ssidVal'),ipVal:$('ipVal'),btnLoadToggle:$('btnLoadToggle'),");
  html += F("btnModeToggle:$('btnModeToggle')};");
  html += F("const prev={};");
  html += F("function text(id,txt){if(prev[id]!==txt){const e=els[id];");
  html += F("if(e)e.textContent=txt;prev[id]=txt;}}");
  html += F("function html(id,content){if(prev[id]!==content){const e=els[id];");
  html += F("if(e)e.innerHTML=content;prev[id]=content;}}");
  html += F("function fill(id,val){const w=(val+'%');if(prev[id]!==w){");
  html += F("const e=els[id];if(e)e.style.width=w;prev[id]=w;}}");
  html += F("function setC(el,cls,on){if(!el)return;");
  html += F("const has=el.classList.contains(cls);");
  html += F("if(on&&!has)el.classList.add(cls);else if(!on&&has)el.classList.remove(cls);}");
  html += F("let pullTimer;");
  html += F("async function pull(){");
  html += F("try{const r=await fetch('/api',{cache:'no-store',");
  html += F("headers:{'X-RequestedWith':'XMLHttpRequest'}});");
  html += F("if(!r.ok)throw new Error('HTTP '+r.status);");
  html += F("const j=await r.json();");
  html += F("text('inputV', j.input_v.toFixed(2)+' V');");
  html += F("text('batV', j.bat_v.toFixed(2)+' V');");
  html += F("text('current', j.current_str);");
  html += F("text('temp', j.temp_c.toFixed(1)+' °C');");
  html += F("text('pwmPct', j.duty_pct+' %'); fill('pwmFill', j.duty_pct);");
  html += F("text('soc', j.soc+'%'); fill('socFill', j.soc);");
  html += F("if(els.batStatus){");
  html += F("const currentClass = els.batStatus.className;");
  html += F("const newClass = 'status-badge ' + j.battery_status_code;");
  html += F("if(currentClass !== newClass){els.batStatus.className = newClass;}");
  html += F("els.batStatus.textContent = j.battery_status;}");
  html += F("text('fanPct', j.fan_pwm_pct+' %'); fill('fanFill', j.fan_pwm_pct);");
  html += F("if(j.mode && j.mode_color){");
  html += F("text('mode', j.mode); els.mode.style.color = j.mode_color;");
  // Dynamic glow class assignment based on mode
  html += F("const modeLower = j.mode.toLowerCase();");
  html += F("const allGlowClasses = ['bulk-glow','absorption-glow','float-glow','backup-glow','backup-batt-glow','backup-idle-glow','idle-glow'];");
  html += F("allGlowClasses.forEach(cls => els.mode.classList.remove(cls));");
  html += F("if(modeLower.includes('bulk')){els.mode.classList.add('bulk-glow');}");
  html += F("else if(modeLower.includes('absorption')){els.mode.classList.add('absorption-glow');}");
  html += F("else if(modeLower.includes('float')){els.mode.classList.add('float-glow');}");
  html += F("else if(modeLower.includes('backup')){");
  html += F("  if(j.mode_description && j.mode_description.toLowerCase().includes('powering')){");
  html += F("    els.mode.classList.add('backup-batt-glow');}");
  html += F("  else if(j.mode_description && j.mode_description.toLowerCase().includes('idle')){");
  html += F("    els.mode.classList.add('backup-idle-glow');}");
  html += F("  else{els.mode.classList.add('backup-glow');}}");
  html += F("else{els.mode.classList.add('idle-glow');}");
  html += F("if(j.mode_description){text('modeDescription', j.mode_description);}}");
  html += F("text('load', j.load_text||j.load);");
  html += F("text('wifiQ', j.wifi_quality); fill('wifiFill', j.wifi_quality);");
  html += F("text('uptimeStr', j.uptime_str);");
  html += F("text('ssidVal', j.ssid||''); text('ipVal', j.ip||'');");
  html += F("const on=!!j.load_on; if(els.btnLoadToggle){");
  html += F("if(prev.loadState!==on){");
  html += F("els.btnLoadToggle.dataset.state=on?'on':'off';");
  html += F("els.btnLoadToggle.textContent=on?'ON':'OFF';");
  html += F("setC(els.btnLoadToggle,'ok',on); setC(els.btnLoadToggle,'danger',!on);");
  html += F("prev.loadState=on;}}");
  html += F("const man=(j.load_mode=='M'); if(els.btnModeToggle){");
  html += F("if(prev.modeState!==man){");
  html += F("els.btnModeToggle.dataset.mode=man?'M':'A';");
  html += F("els.btnModeToggle.textContent=man?'MANUAL':'AUTO';");
  html += F("setC(els.btnModeToggle,'secondary',man);");
  html += F("setC(els.btnModeToggle,'primary',!man); prev.modeState=man;}}");
  html += F("}catch(e){console.error('API fetch failed:', e);}");
  html += F("schedule();}");
  html += F("function schedule(){clearTimeout(pullTimer);");
  html += F("const interval=document.hidden?100:20;");
  html += F("pullTimer=setTimeout(pull,interval);}");
  html += F("document.addEventListener('visibilitychange',schedule);");
  html += F("document.addEventListener('DOMContentLoaded', ()=>{schedule(); pull();});");
  html += F("if(document.readyState==='loading'){");
  html += F("document.addEventListener('DOMContentLoaded', pull);}else{pull();}");
  html += F("async function post(u){try{await fetch(u,{method:'POST',");
  html += F("headers:{'X-RequestedWith':'XMLHttpRequest'}});setTimeout(pull,10);}catch(e){}}");
  html += F("const lt=$('btnLoadToggle'), mt=$('btnModeToggle');");
  html += F("if(lt)lt.onclick=()=>post((lt.dataset.state==='on')?'/api/loadoff':'/api/loadon');");
  html += F("if(mt)mt.onclick=()=>post((mt.dataset.mode==='M')?'/api/auto':'/api/manual');");
  html += F("function sleep(ms){return new Promise(r=>setTimeout(r,ms));}");
  html += F("async function restartAndHome(){");
  html += F("const ov=$('overlay'), ot=$('ovText'), os=$('ovSec'); let sec=60, cdt;");
  html += F("if(ov){ov.style.display='flex';}");
  html += F("if(ot){ot.textContent='Restarting… trying to reconnect';}");
  html += F("if(os){os.textContent=sec; cdt=setInterval(()=>{sec--;");
  html += F("if(sec>=0) os.textContent=sec;");
  html += F("if(sec<=0){clearInterval(cdt); location.replace(MDNS_URL+'/');}},1000);}");
  html += F("try{await fetch('/api/restart',{method:'POST'});}catch(e){}");
  html += F("await sleep(500); const res=await pingEither(60000,2000);");
  html += F("if(res){ if(cdt) clearInterval(cdt);");
  html += F("location.replace(res==='mdns'?MDNS_URL+'/':'/'); }}");
  html += F("function mdnsAllowedHost(){try{return location.hostname!==MDNS_HOST;}");
  html += F("catch(e){return true;}}");
  html += F("async function mdnsProbe(timeoutMs=1500){");
  html += F("const url=MDNS_URL+'/api';");
  html += F("if(!('AbortController' in window)){");
  html += F("try{await fetch(url,{cache:'no-store',mode:'no-cors'}); return true;}");
  html += F("catch(e){return false;}}");
  html += F("const ctl=new AbortController(); const to=setTimeout(()=>ctl.abort(),timeoutMs);");
  html += F("try{ await fetch(url,{cache:'no-store',mode:'no-cors',signal:ctl.signal});");
  html += F("clearTimeout(to); return true; }catch(e){ clearTimeout(to); return false;}}");
  html += F("let __mdnsAutoTimer;");
  html += F("function scheduleMDNSAuto(){clearTimeout(__mdnsAutoTimer);");
  html += F("__mdnsAutoTimer=setTimeout(async()=>{");
  html += F("if(document.hidden){ scheduleMDNSAuto(); return; }");
  html += F("if(mdnsAllowedHost()){ const ok=await mdnsProbe();");
  html += F("if(ok){ location.replace(MDNS_URL+'/'); return; } }");
  html += F("scheduleMDNSAuto();}, document.hidden?12000:8000);}");
  html += F("document.addEventListener('visibilitychange', scheduleMDNSAuto);");
  html += F("scheduleMDNSAuto();");
  html += F("const rs=$('btnRestart'), wc=$('btnWiFiCfg');");
  html += F("if(rs)rs.onclick=()=>{ if(confirm('Restart the device now?')) restartAndHome(); };");
  html += F("if(wc)wc.onclick=()=>{ if(confirm('Open WiFi configuration page?'))");
  html += F(" location.href='/wifi'; };");
  html += F("</script>");

  html += "</div></body></html>";
  return html;
}

//*****************************************************************************
// JSON API Builder
//*****************************************************************************

String makeJSON() {
  // Update cached network info
  updateNetworkInfo();

  const int duty = dutyPercentCapAware();
  const int soc = constrain(getBatteryPercentage(bat_voltage), 0, 100);
  const int wifiQ = constrain(getWiFiSignalQuality(), 0, 100);
  const String wifiBars = getWiFiBars(wifiQ);
  const bool isManual = (loadControlModeStr == "M");
  const int fanDutyPct = (FAN_MAX_DUTY > 0) ?
                         constrain((getFanDuty() * 100) / FAN_MAX_DUTY, 0, 100) : 0;

  const String loadText = formatLoadText(load_is_on, isManual);
  const String batStatusText = getBatteryStatusText(soc);
  const char* batStatusCode = getBatteryStatusCode(soc);
  const int uptimeSec = getUptimeSeconds();
  const String uptimeStr = formatUptimeString();

  // Get mode configuration directly from original mode_str
  const ModeConfig* currentModeConfig = getModeConfig(mode_str);

  // CRITICAL FIX: Always include mode color and description
  String modeColor = String(currentModeConfig->color);
  String modeDisplay = String(currentModeConfig->displayName);
  String modeDescription = String(currentModeConfig->description);

  // Safety validation for mode color and description
  if (modeColor.length() == 0) modeColor = "#e6edf3"; // Fallback
  if (modeDisplay.length() == 0) modeDisplay = "IDLE";
  if (modeDescription.length() == 0) modeDescription = "Ready To Work";

  String json;
  json.reserve(JSON_RESERVE_SIZE);

  json += "{";
  // numeric metrics
  json += "\"input_v\":" + String(input_voltage, 2) + ",";
  json += "\"bat_v\":" + String(bat_voltage, 2) + ",";
  json += "\"current_a\":" + String(Current_A, 3) + ",";
  // human-friendly current string
  json += "\"current_str\":\"" + jsonEscape(formatCurrentStr()) + "\",";
  json += "\"temp_c\":" + String(temperatureC, 1) + ",";
  json += "\"duty_pct\":" + String(duty) + ",";
  // fan PWM %
  json += "\"fan_pwm_pct\":" + String(fanDutyPct) + ",";
  json += "\"soc\":" + String(soc) + ",";
  // mode label (string) - use the full display name
  json += "\"mode\":\"" + jsonEscape(modeDisplay) + "\",";
  // mode color and description for UI display
  json += "\"mode_color\":\"" + modeColor + "\",";
  json += "\"mode_description\":\"" + jsonEscape(modeDescription) + "\",";

  // battery status (human + code)
  json += "\"battery_status\":\"" + jsonEscape(batStatusText) + "\",";
  json += "\"battery_status_code\":\"" + String(batStatusCode) + "\",";

  // Legacy fields for backwards compatibility
  json += "\"load\":\"" + jsonEscape(load_status) + "\",";
  json += "\"load_mode\":\"" + jsonEscape(loadControlModeStr) + "\",";
  // strongly-typed fields
  json += "\"load_on\":";
  json += (load_is_on ? "true" : "false");
  json += ",";
  json += "\"load_text\":\"" + jsonEscape(loadText) + "\",";
  // WiFi info
  json += "\"wifi_quality\":" + String(wifiQ) + ",";
  json += "\"wifi_bars\":\"" + wifiBars + "\",";
  // Uptime tracking
  json += "\"uptime_sec\":" + String(uptimeSec) + ",";
  json += "\"uptime_str\":\"" + jsonEscape(uptimeStr) + "\",";
  // Network info
  json += "\"ssid\":\"" + jsonEscape(wifiSSID) + "\",";
  json += "\"ip\":\"" + jsonEscape(localIP) + "\"";
  json += "}";

  return json;
}

//*****************************************************************************
// Web Server Route Setup
//*****************************************************************************

void setupWebServer() {
  initUptime();
  setupWebServerRoutes();
  server.begin();
}

//************************************************************************************
// handleWebServer - Process incoming web server requests
// Purpose: Pump server - call frequently in loop()
//************************************************************************************
void handleWebServer() {
  server.handleClient();
}
