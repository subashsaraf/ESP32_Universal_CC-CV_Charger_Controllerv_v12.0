//************************************************************************************
// charger_web_routes.cpp
//
// Web server route registration for ESP32 Universal CC/CV Charger Controller.
//
// Responsibilities:
//   - Registers all HTTP routes (HTML pages + JSON APIs)
//   - Wires UI actions to backend control logic
//   - Applies security headers and CSRF protection
//   - Handles WiFi configuration, scanning, and reboot flow
//
// This file contains ONLY route definitions.
// No HTML generation or business logic is implemented here.
//
// Dependencies:
//   - charger_web.h      (HTML/JSON builders, helpers, globals)
//   - WebServer (ESP32 core)
//
// Called from:
//   setupWebServer() or equivalent initialization function
//
//   - Updated to reference charger_web.h 
//************************************************************************************

#include "charger_web.h"

void setupWebServerRoutes() {
  // Dashboard (HTML)
  server.on("/", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    addSecurityHeaders();
    server.send(200, "text/html; charset=utf-8", makeHTML());
  });

  // Status API (polled by UI)
  server.on("/api", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    addSecurityHeaders();
    server.send(200, "application/json; charset=utf-8", makeJSON());
  });

  // Actions: Load ON
  server.on("/api/loadon", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "application/json", F("{\"ok\":false,\"err\":\"csrf\"}")); return; }
    applyLoad(true);
    addSecurityHeaders();
    server.send(200, "application/json", F("{\"ok\":true}"));
  });

  // Actions: Load OFF
  server.on("/api/loadoff", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "application/json", F("{\"ok\":false,\"err\":\"csrf\"}")); return; }
    applyLoad(false);
    addSecurityHeaders();
    server.send(200, "application/json", F("{\"ok\":true}"));
  });

  // Actions: AUTO mode
  server.on("/api/auto", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "application/json", F("{\"ok\":false,\"err\":\"csrf\"}")); return; }
    loadControlMode = AUTO; loadControlModeStr = "A"; persistLoadMode();
    addSecurityHeaders();
    server.send(200, "application/json", F("{\"ok\":true}"));
  });

  // Actions: MANUAL mode
  server.on("/api/manual", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "application/json", F("{\"ok\":false,\"err\":\"csrf\"}")); return; }
    loadControlMode = MANUAL; loadControlModeStr = "M"; persistLoadMode();
    addSecurityHeaders();
    server.send(200, "application/json", F("{\"ok\":true}"));
  });

  // Restart (respond then reboot)
  server.on("/api/restart", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "application/json", F("{\"ok\":false,\"err\":\"csrf\"}")); return; }
    server.sendHeader("Connection", "close");
    addSecurityHeaders();
    server.send(200, "application/json", F("{\"ok\":true,\"restarting\":true}"));
    delay(200); ESP.restart();
  });

  // WiFi page (UI)
  server.on("/wifi", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    addSecurityHeaders();
    server.send(200, "text/html; charset=utf-8", makeWiFiHTML());
  });

  // Scanner API (returns up to 20 visible networks; skips hidden SSIDs)
  server.on("/api/scan", HTTP_GET, []() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    int n = WiFi.scanNetworks(false, false);
    if (n == WIFI_SCAN_FAILED) {
      String json = F("{\"error\":\"scan_failed\",\"nets\":[],\"count\":0}");
      addSecurityHeaders();
      server.send(200, "application/json; charset=utf-8", json);
      return;
    }
    if (n < 0) n = 0;

    String json; json.reserve(2000);
    json += "{\"nets\":[";
    int emitted = 0;
    for (int i = 0; i < n && emitted < 20; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      if (emitted++) json += ",";
      json += "{\"ssid\":\"" + jsonEscape(ssid) + "\","
              "\"rssi\":" + String(WiFi.RSSI(i)) + ","
              "\"ch\":"   + String(WiFi.channel(i)) + ","
              "\"enc\":\""+ encToStr((uint8_t)WiFi.encryptionType(i)) + "\"}";
    }
    json += "],\"count\":" + String(emitted) + "}";
    WiFi.scanDelete();

    addSecurityHeaders();
    server.send(200, "application/json; charset=utf-8", json);
  });

  // Save WiFi + reboot (AJAX or fallback HTML)
  server.on("/api/wifi_save", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "application/json; charset=utf-8", F("{\"ok\":false,\"error\":\"csrf\"}")); return; }

    const String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
    const String pass = server.hasArg("pass") ? server.arg("pass") : "";
    if (ssid.length() == 0) {
      addSecurityHeaders();
      server.send(400, "application/json; charset=utf-8", F("{\"ok\":false,\"error\":\"ssid_required\"}"));
      return;
    }

    WiFi.mode(WIFI_STA); WiFi.persistent(true); WiFi.begin(ssid.c_str(), pass.c_str());

    bool wantsJSON = server.hasHeader("Accept") && server.header("Accept").indexOf("application/json") >= 0;
    if (wantsJSON) {
      String j = String("{\"ok\":true,\"ssid\":\"") + jsonEscape(ssid) + "\"}";
      server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
      server.sendHeader("Connection", "close");
      addSecurityHeaders();
      server.send(200, "application/json; charset=utf-8", j);
      delay(300); ESP.restart();
      return;
    }

    const String ssidSafe = htmlEscape(ssid);
    const String ipSafe   = htmlEscape(WiFi.localIP().toString());
    const String mdnsURL  = "http://" + String(MDNS_HOST) + ".local";
    String resp; resp.reserve(2400);
    resp += F("<!DOCTYPE html><html><head><meta charset='utf-8'>"
              "<meta name='viewport' content='width=device-width,initial-scale=1'>"
              "<meta name='color-scheme' content='dark'>"
              "<title>Saving WiFi</title>"
              "<style>"
              "body{margin:0;background:#0d1117;color:#e6edf3;font-family:system-ui,-apple-system,Segoe UI,Roboto,Arial}"
              ".wrap{min-height:100svh;display:grid;place-items:center;padding:16px}"
              ".box{background:#0f1623;border:1px solid #1f2a3b;border-radius:12px;padding:18px 20px;max-width:520px;text-align:center}"
              ".t{font-weight:800;font-size:18px;margin:2px 0 6px}.m{opacity:.85;font-size:13px}"
              ".spin{width:28px;height:28px;border:3px solid #2a2f3a;border-top-color:#60a5fa;border-radius:50%;margin:10px auto 10px;animation:sp 1s linear infinite}"
              "@keyframes sp{to{transform:rotate(360deg)}}"
              "</style></head><body><div class='wrap'><div class='box'>"
              "<div class='t'>WiFi settings saved</div>"
              "<div class='m'>Saved SSID: <strong>");
    resp += ssidSafe;
    resp += F("</strong></div>"
              "<div class='m'>IP Address: <strong>");
    resp += ipSafe;
    resp += F("</strong></div>"
              "<div class='m'>Rebooting now… this page will return to Home automatically when the device is back online.</div>"
              "<div class='spin'></div>"
              "<div class='m'>Returning to Home in <span id='cd'>120</span>s or sooner if connected.</div>"
              "<div class='m' id='note' style='margin-top:6px'></div>"
              "</div></div>"
              "<script>"
              "const MDNS_URL='");
    resp += mdnsURL + F("';"
              "function sleep(ms){return new Promise(r=>setTimeout(r,ms));"
              "async function pingEither(ms=120000,step=1000){"
                "const start=Date.now();"
                "while(Date.now()-start<ms){"
                  "try{const r=await fetch('/api',{cache:'no-store',headers:{'X-RequestedWith':'XMLHttpRequest'}}); if(r.ok) return 'local';}catch(e){}"
                  "try{"
                    "if('AbortController' in window){"
                      "const c=new AbortController(); const to=setTimeout(()=>c.abort(),800);"
                      "await fetch(MDNS_URL+'/api',{cache:'no-store',mode:'no-cors',signal:c.signal});"
                      "clearTimeout(to); return 'mdns';"
                    "}else{ await fetch(MDNS_URL+'/api',{cache:'no-store',mode:'no-cors'}); return 'mdns'; }"
                  "}catch(e){}"
                  "await sleep(step);"
                "}"
                "return false;"
              "}"
              "(async()=>{"
                "let sec=120; const el=document.getElementById('cd');"
                "const int=setInterval(()=>{sec--; if(el) el.textContent=sec; if(sec<=0){clearInterval(int); location.replace('/');}},1000);"
                "await sleep(600);"
                "const res=await pingEither(120000,1000);"
                "if(res){clearInterval(int); location.replace(res==='mdns'?MDNS_URL+'/':'/');}"
                "else{const n=document.getElementById('note'); if(n) n.textContent='If this page does not reload, try MDNS or the new IP.';}"
              "})();"
              "</script></body></html>");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Connection", "close");
    addSecurityHeaders();
    server.send(200, "text/html; charset=utf-8", resp);
    delay(300); ESP.restart();
  });

  // Legacy POST routes kept for compatibility (redirect to root)
  server.on("/loadon", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "text/plain", "Forbidden"); return; }
    applyLoad(true);
    server.sendHeader("Location", "/", true); addSecurityHeaders();
    server.send(303, "text/plain", "");
  });
  server.on("/loadoff", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "text/plain", "Forbidden"); return; }
    applyLoad(false);
    server.sendHeader("Location", "/", true); addSecurityHeaders();
    server.send(303, "text/plain", "");
  });
  server.on("/auto", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "text/plain", "Forbidden"); return; }
    loadControlMode = AUTO; loadControlModeStr = "A"; persistLoadMode();
    server.sendHeader("Location", "/", true); addSecurityHeaders();
    server.send(303, "text/plain", "");
  });
  server.on("/manual", HTTP_POST, []() {
    if (!csrfOK()) { addSecurityHeaders(); server.send(403, "text/plain", "Forbidden"); return; }
    loadControlMode = MANUAL; loadControlModeStr = "M"; persistLoadMode();
    server.sendHeader("Location", "/", true); addSecurityHeaders();
    server.send(303, "text/plain", "");
  });

  // Favicon: return 204 to silence browser 404s
  server.on("/favicon.ico", HTTP_GET, []() { addSecurityHeaders(); server.send(204); });

  // Not found handler
  server.onNotFound([]() {
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    addSecurityHeaders();
    server.send(404, "text/plain; charset=utf-8", "Not Found");
  });

  server.begin();
}
