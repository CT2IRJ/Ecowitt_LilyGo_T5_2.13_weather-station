#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
SET_LOOP_TASK_STACK_SIZE(16384);

// LilyGo T5 2.13" V2.3 / V2.4 Pinout
#define EPD_CS     5
#define EPD_DC     17
#define EPD_RST    16
#define EPD_BUSY   4
#define EPD_POWER  12 // Used on V2.4 boards; harmless on V2.3
#define BTN_CONFIG 39 // Onboard programmable button (IO39)

// Display driver for DEPG0213BN / GDEM0213B74 (122x250 B&W)
GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(GxEPD2_213_B74(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

Preferences prefs;
WebServer server(80);

// Configuration variables (input here your details)
String appKey = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX";  //Ecowitt application Key 32 Chars
String apiKey = "";  //Ecowitt API Key
String macAddr = "XX:XX:XX:XX:XX:XX";  //Ecowitt Weather Station MAC Address
int refreshMins = 15;  // Intervalo de actualização

unsigned long lastFetch = 0;
bool forceRefresh = true;

// Weather Data Structure
struct WeatherData {
  String temp = "--";
  String feelsLike = "--";
  String dewPoint = "--";
  String humidity = "--";
  String wind = "--";
  String windGust = "--";
  String windDir = "--";
  String windDeg = "--";
  String rain = "--";
  String rainRate = "--";
  String solar = "--";
  String uvi = "--";
  String pressure = "--";
  String indoorTemp = "--";
  String indoorHum = "--";
  String battery = "--";
  String status = "OK";
  String lastUpdate = "--/-- --:--";
} weather;
#define MAX_HISTORY 96 // 24 horas x 4 leituras/hora (15 min)

struct HistoryPoint {
  char time[6];   // "HH:MM"
  float temp;
  float hum;
  float press;
};

HistoryPoint history[MAX_HISTORY];
int histCount = 0;

void addHistoryPoint(const String& timeStr, const String& t, const String& h, const String& p) {
  if (t == "--" || t.length() == 0) return;

  // Evita duplicar se o utilizador clicar em "Atualizar" no mesmo minuto
  String hhmm = (timeStr.length() >= 11) ? timeStr.substring(6, 11) : "--:--";
  if (histCount > 0 && strcmp(history[histCount - 1].time, hhmm.c_str()) == 0) {
    history[histCount - 1].temp = t.toFloat();
    history[histCount - 1].hum = h.toFloat();
    history[histCount - 1].press = p.toFloat();
    return;
  }

  if (histCount < MAX_HISTORY) {
    strncpy(history[histCount].time, hhmm.c_str(), 5);
    history[histCount].time[5] = '\0';
    history[histCount].temp = t.toFloat();
    history[histCount].hum = h.toFloat();
    history[histCount].press = p.toFloat();
    histCount++;
  } else {
    for (int i = 1; i < MAX_HISTORY; i++) {
      history[i - 1] = history[i];
    }
    strncpy(history[MAX_HISTORY - 1].time, hhmm.c_str(), 5);
    history[MAX_HISTORY - 1].time[5] = '\0';
    history[MAX_HISTORY - 1].temp = t.toFloat();
    history[MAX_HISTORY - 1].hum = h.toFloat();
    history[MAX_HISTORY - 1].press = p.toFloat();
  }
}


String lastDebugInfo = "No fetch yet";

// ================= IPMA alerts =================
#define ALERT_MIN_LEVEL   1                  // 1=amarelo, 2=laranja, 3=vermelho (nível mínimo para inverter campos)
#define ALERT_INTERVAL_MS 600000UL           // consultar o IPMA no máx. de 10 em 10 minutos
#define MAX_ALERTS        8
#define IPMA_WARN_URL     "https://api.ipma.pt/open-data/forecast/warnings/warnings_www.json"

struct AlertItem {
  uint8_t level = 0;      // 1=yellow 2=orange 3=red
  bool active = false;    // já começou e ainda não terminou
  String type;            // awarenessTypeName (ex.: "Precipitação")
  String start;
  String end;
  String text;
};

String district = "STM";                     // idAreaAviso (Santarém por omissão)
AlertItem alerts[MAX_ALERTS];
int alertCount = 0;
uint8_t fieldLevel[5] = {0, 0, 0, 0, 0};     // 1=Temp 2=Wind 3=Rain 4=Hum  (nível de aviso ativo)
String alertFooter = "";                     // texto do rodapé do e-paper (vazio = sem aviso)
String alertStatus = "--";
unsigned long lastAlertFetch = 0;
bool forceAlerts = true;

void loadConfig() {
  prefs.begin("ecowitt", true);
  appKey = prefs.getString("appKey", "");
  apiKey = prefs.getString("apiKey", "");
  macAddr = prefs.getString("macAddr", "");
  refreshMins = prefs.getInt("refresh", 15);
  district = prefs.getString("district", "STM");
  prefs.end();
}

void saveConfig() {
  prefs.begin("ecowitt", false);
  prefs.putString("appKey", appKey);
  prefs.putString("apiKey", apiKey);
  prefs.putString("macAddr", macAddr);
  prefs.putInt("refresh", refreshMins);
  prefs.putString("district", district);
  prefs.end();
}

// Local Web Configuration Homepage (accessible at http://<ESP32-IP>/)
// Endpoint JSON para atualização dinâmica do dashboard
void handleApiData() {
  JsonDocument doc;
  doc["temp"] = weather.temp;
  doc["feelsLike"] = weather.feelsLike;
  doc["dewPoint"] = weather.dewPoint;
  doc["humidity"] = weather.humidity;
  doc["wind"] = weather.wind;
  doc["windGust"] = weather.windGust;
  doc["windDir"] = weather.windDir;
  doc["windDeg"] = weather.windDeg;
  doc["rain"] = weather.rain;
  doc["rainRate"] = weather.rainRate;
  doc["solar"] = weather.solar;
  doc["uvi"] = weather.uvi;
  doc["pressure"] = weather.pressure;
  doc["indoorTemp"] = weather.indoorTemp;
  doc["indoorHum"] = weather.indoorHum;
  doc["battery"] = weather.battery;
  doc["lastUpdate"] = weather.lastUpdate;

  // Memória do ESP32 (KB)
  doc["freeHeap"]  = String(ESP.getFreeHeap() / 1024.0, 1);
  doc["minHeap"]   = String(ESP.getMinFreeHeap() / 1024.0, 1);
  doc["heapTotal"] = String(ESP.getHeapSize() / 1024.0, 0);

  // Avisos IPMA
  doc["district"] = district;
  doc["alertStatus"] = alertStatus;
  JsonArray al = doc["alerts"].to<JsonArray>();
  const char* lvlName[] = {"green", "yellow", "orange", "red"};
  for (int i = 0; i < alertCount; i++) {
    JsonObject o = al.add<JsonObject>();
    o["l"] = lvlName[alerts[i].level];
    o["t"] = alerts[i].type;
    o["s"] = alerts[i].start;
    o["e"] = alerts[i].end;
    o["x"] = alerts[i].text;
    o["a"] = alerts[i].active;
  }

  JsonArray arr = doc["history"].to<JsonArray>();
  for (int i = 0; i < histCount; i++) {
    JsonObject pt = arr.add<JsonObject>();
    pt["t"] = history[i].time;
    pt["temp"] = history[i].temp;
    pt["hum"] = history[i].hum;
    pt["press"] = history[i].press;
  }

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleRoot() {
  if (apiKey.length() < 5) {
    server.sendHeader("Location", "/setup", true);
    server.send(302, "text/plain", "");
    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html><html lang="pt"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Estação Meteorológica</title>
<style>
  :root { --bg:#f1f5f9; --card:#ffffff; --text:#0f172a; --muted:#64748b; --accent:#0284c7; --border:#e2e8f0; }
  @media (prefers-color-scheme: dark) {
    :root { --bg:#0f172a; --card:#1e293b; --text:#f8fafc; --muted:#94a3b8; --accent:#38bdf8; --border:#334155; }
  }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif; background:var(--bg); color:var(--text); padding:20px; }
  .wrap { max-width:900px; margin:0 auto; }
  header { display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px; margin-bottom:24px; }
  h1 { font-size:1.4rem; font-weight:700; }
  .sub { font-size:0.85rem; color:var(--muted); margin-top:4px; }
  .actions, .tabs { display:flex; gap:8px; }
  .btn, .tab { background:var(--card); color:var(--text); border:1px solid var(--border); padding:8px 14px; border-radius:10px; text-decoration:none; font-size:0.85rem; font-weight:600; cursor:pointer; transition:0.2s; }
  .btn:hover, .tab:hover { border-color:var(--accent); color:var(--accent); }
  .tab.active { background:var(--accent); color:#fff; border-color:var(--accent); }
  .grid { display:grid; grid-template-columns:repeat(auto-fit, minmax(210px, 1fr)); gap:16px; margin-bottom:20px; }
  .card { background:var(--card); border:1px solid var(--border); border-radius:16px; padding:18px; box-shadow:0 2px 8px rgba(0,0,0,0.04); }
  .label { font-size:0.78rem; text-transform:uppercase; letter-spacing:0.05em; color:var(--muted); font-weight:700; }
  .val { font-size:2rem; font-weight:700; margin:8px 0 4px; color:var(--text); }
  .unit { font-size:1rem; font-weight:500; color:var(--muted); }
  .extra { font-size:0.84rem; color:var(--muted); }
  .chart-head { display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:10px; margin-bottom:14px; }
  .stats { font-size:0.82rem; color:var(--muted); margin-top:4px; }
  svg { width:100%; height:190px; overflow:visible; display:block; }
  .ipma { margin-bottom:20px; }
  .ipma-row { display:flex; flex-wrap:wrap; gap:10px; align-items:center; margin:10px 0 14px; }
  select { background:var(--card); color:var(--text); border:1px solid var(--border); padding:8px 10px; border-radius:10px; font-size:0.85rem; font-weight:600; }
  .alert { border-left:6px solid var(--muted); background:var(--bg); border-radius:8px; padding:10px 12px; margin-top:8px; font-size:0.88rem; }
  .alert.yellow { border-color:#eab308; } .alert.orange { border-color:#f97316; } .alert.red { border-color:#dc2626; }
  .alert.soon { opacity:0.65; }
  .alert b { display:block; margin-bottom:2px; }
  .alert small { color:var(--muted); }
  .noalert { font-size:0.88rem; color:var(--muted); }
  .foot { text-align:center; font-size:0.75rem; color:var(--muted); margin-top:18px; }
</style></head><body>
<div class="wrap">
  <header>
    <div>
      <h1>🌤️ Estação Meteorológica</h1>
      <div class="sub">Última leitura: <strong id="lastUpdate">--</strong></div>
    </div>
    <div class="actions">
      <button class="btn" onclick="forceSync()">🔄 Atualizar</button>
      <a href="/setup" class="btn">⚙️ Setup</a>
    </div>
  </header>
  <div class="grid">
    <div class="card">
      <div class="label">Temperatura Exterior</div>
      <div class="val"><span id="temp">--</span><span class="unit"> °C</span></div>
      <div class="extra">Sensação: <span id="feelsLike">--</span> °C &bull; Orvalho: <span id="dewPoint">--</span> °C</div>
    </div>
    <div class="card">
      <div class="label">Humidade Exterior</div>
      <div class="val"><span id="humidity">--</span><span class="unit"> %</span></div>
      <div class="extra">Interior: <span id="indoorTemp">--</span> °C &bull; <span id="indoorHum">--</span> %</div>
    </div>
    <div class="card">
      <div class="label">Vento</div>
      <div class="val"><span id="wind">--</span><span class="unit"> km/h</span></div>
      <div class="extra">Dir: <span id="windDir">--</span> (<span id="windDeg">--</span>°) &bull; Rajada: <span id="windGust">--</span> km/h</div>
    </div>
    <div class="card">
      <div class="label">Precipitação</div>
      <div class="val"><span id="rain">--</span><span class="unit"> mm</span></div>
      <div class="extra">Taxa atual: <span id="rainRate">--</span> mm/h</div>
    </div>
    <div class="card">
      <div class="label">Radiação Solar & UV</div>
      <div class="val"><span id="solar">--</span><span class="unit"> W/m²</span></div>
      <div class="extra">Índice UV: <span id="uvi">--</span></div>
    </div>
    <div class="card">
      <div class="label">Pressão & Sistema</div>
      <div class="val"><span id="pressure">--</span><span class="unit"> hPa</span></div>
      <div class="extra">Bateria Estação: <strong id="battery">--</strong></div>
    </div>
  </div>
  <div class="card ipma">
    <div class="label">IPMA &bull; Previsão e Avisos</div>
    <div class="ipma-row">
      <select id="district" onchange="setDistrict(this.value)"></select>
      <a class="btn" href="https://www.ipma.pt/pt/otempo/prev.significativa/" target="_blank" rel="noopener">🌦️ Previsão significativa</a>
      <a class="btn" href="https://www.ipma.pt/pt/html.sam.jsp" target="_blank" rel="noopener">⚠️ Avisos (IPMA)</a>
    </div>
    <div id="alerts" class="noalert">A carregar avisos...</div>
  </div>
  <div class="card">
    <div class="chart-head">
      <div>
        <div class="label">Evolução nas Últimas 24 Horas</div>
        <div class="stats" id="chartStats">A aguardar histórico...</div>
      </div>
      <div class="tabs">
        <button class="tab active" onclick="setMetric('temp','°C',this)">Temp °C</button>
        <button class="tab" onclick="setMetric('hum','%',this)">Hum %</button>
        <button class="tab" onclick="setMetric('press','hPa',this)">hPa</button>
      </div>
    </div>
    <svg id="chart" viewBox="0 0 800 180" preserveAspectRatio="none"></svg>
  </div>
  <div class="foot">Memória livre: <span id="freeHeap">--</span> KB (mín. <span id="minHeap">--</span> KB) de <span id="heapTotal">--</span> KB</div>
</div>
<script>
  let histData = [], curMetric = 'temp', curUnit = '°C';

  function setMetric(m, u, btn) {
    curMetric = m; curUnit = u;
    document.querySelectorAll('.tab').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    renderChart();
  }

  function renderChart() {
    const svg = document.getElementById('chart');
    const stats = document.getElementById('chartStats');
    if (!histData || histData.length < 2) {
      svg.innerHTML = '<text x="400" y="95" text-anchor="middle" fill="var(--muted)" font-size="14">São necessárias pelo menos 2 leituras para desenhar o gráfico.</text>';
      stats.textContent = `Registos guardados: ${histData ? histData.length : 0} / 96`;
      return;
    }
    const vals = histData.map(d => d[curMetric]);
    const min = Math.min(...vals), max = Math.max(...vals);
    const avg = (vals.reduce((a,b)=>a+b,0)/vals.length).toFixed(1);
    stats.textContent = `Mín: ${min.toFixed(1)} ${curUnit}  •  Méd: ${avg} ${curUnit}  •  Máx: ${max.toFixed(1)} ${curUnit}`;

    const padL = 48, padR = 12, padT = 14, padB = 26, W = 800 - padL - padR, H = 180 - padT - padB;
    const span = (max - min) || 1;
    const pts = histData.map((d, i) => {
      const x = padL + (i / (histData.length - 1)) * W;
      const y = padT + H - ((d[curMetric] - min) / span) * H;
      return `${x.toFixed(1)},${y.toFixed(1)}`;
    });

    const firstX = padL, lastX = padL + W, baseY = padT + H;
    const areaPts = `${firstX},${baseY} ` + pts.join(' ') + ` ${lastX},${baseY}`;
    const midVal = ((max + min) / 2).toFixed(1);
    const tStart = histData[0].t, tMid = histData[Math.floor(histData.length/2)].t, tEnd = histData[histData.length-1].t;

    svg.innerHTML = `
      <defs>
        <linearGradient id="g" x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stop-color="var(--accent)" stop-opacity="0.28"/>
          <stop offset="100%" stop-color="var(--accent)" stop-opacity="0.0"/>
        </linearGradient>
      </defs>
      <line x1="${padL}" y1="${padT}" x2="${lastX}" y2="${padT}" stroke="var(--border)" stroke-dasharray="4"/>
      <line x1="${padL}" y1="${padT+H/2}" x2="${lastX}" y2="${padT+H/2}" stroke="var(--border)" stroke-dasharray="4"/>
      <line x1="${padL}" y1="${baseY}" x2="${lastX}" y2="${baseY}" stroke="var(--border)"/>
      <text x="${padL-8}" y="${padT+4}" text-anchor="end" fill="var(--muted)" font-size="11">${max.toFixed(1)}</text>
      <text x="${padL-8}" y="${padT+H/2+4}" text-anchor="end" fill="var(--muted)" font-size="11">${midVal}</text>
      <text x="${padL-8}" y="${baseY+4}" text-anchor="end" fill="var(--muted)" font-size="11">${min.toFixed(1)}</text>
      <text x="${firstX}" y="174" text-anchor="start" fill="var(--muted)" font-size="11">${tStart}</text>
      <text x="${padL+W/2}" y="174" text-anchor="middle" fill="var(--muted)" font-size="11">${tMid}</text>
      <text x="${lastX}" y="174" text-anchor="end" fill="var(--muted)" font-size="11">${tEnd}</text>
      <polygon points="${areaPts}" fill="url(#g)"/>
      <polyline fill="none" stroke="var(--accent)" stroke-width="2.5" stroke-linejoin="round" points="${pts.join(' ')}"/>
    `;
  }

  function loadData() {
    fetch('/api/data').then(r => r.json()).then(d => {
      for (const [k, v] of Object.entries(d)) {
        if (k === 'history') { histData = v; renderChart(); continue; }
        if (k === 'alerts') { lastAlerts = v; continue; }
        if (k === 'alertStatus') { lastAlertStatus = v; continue; }
        if (k === 'district') { document.getElementById('district').value = v; continue; }
        const el = document.getElementById(k);
        if (el) el.textContent = v;
      }
      renderAlerts();
    });
  }
  function forceSync() {
    document.getElementById('lastUpdate').textContent = 'A sincronizar...';
    fetch('/refresh').then(() => setTimeout(loadData, 2500));
  }

  // ---- IPMA: distrito e avisos (o ESP32 faz o pedido ao IPMA) ----
  const DISTRICTS = [
    ['AVR','Aveiro'],['BJA','Beja'],['BRG','Braga'],['BGC','Bragança'],['CBO','Castelo Branco'],
    ['CBR','Coimbra'],['EVR','Évora'],['FAR','Faro'],['GDA','Guarda'],['LRA','Leiria'],
    ['LSB','Lisboa'],['PTG','Portalegre'],['PRT','Porto'],['STM','Santarém'],['STB','Setúbal'],
    ['VCT','Viana do Castelo'],['VRL','Vila Real'],['VIS','Viseu']
  ];
  const LEVELS = { yellow:'Amarelo', orange:'Laranja', red:'Vermelho' };
  let lastAlerts = [], lastAlertStatus = '--';

  document.getElementById('district').innerHTML =
    DISTRICTS.map(d => `<option value="${d[0]}"${d[0] === 'STM' ? ' selected' : ''}>${d[1]}</option>`).join('');

  function esc(t) { return String(t == null ? '' : t).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c])); }
  function fmt(iso) {
    const d = new Date(iso);
    return isNaN(d) ? '' : d.toLocaleString('pt-PT', { day:'2-digit', month:'2-digit', hour:'2-digit', minute:'2-digit' });
  }
  function renderAlerts() {
    const box = document.getElementById('alerts');
    if (lastAlertStatus !== 'OK' && !lastAlerts.length) {
      box.className = 'noalert';
      box.textContent = lastAlertStatus === '--' ? 'A aguardar dados do IPMA...' : 'IPMA: ' + lastAlertStatus;
      return;
    }
    if (!lastAlerts.length) { box.className = 'noalert'; box.textContent = '✅ Sem avisos para este distrito.'; return; }
    box.className = '';
    box.innerHTML = lastAlerts.map(w => `<div class="alert ${esc(w.l)}${w.a ? '' : ' soon'}">
      <b>${esc(LEVELS[w.l] || w.l)} &bull; ${esc(w.t)}${w.a ? '' : ' (em breve)'}</b>
      ${esc(w.x)}<br><small>${fmt(w.s)} → ${fmt(w.e)}</small></div>`).join('');
  }
  function setDistrict(code) {
    document.getElementById('alerts').textContent = 'A atualizar avisos...';
    fetch('/setdistrict?code=' + encodeURIComponent(code)).then(() => setTimeout(loadData, 8000));
  }

  loadData();
  setInterval(loadData, 30000);
</script>
</body></html>
)rawliteral";

  server.send(200, "text/html", html);
}


// Página de Configuração (acessível em http://<ESP32-IP>/setup)
void handleSetup() {
  String html = "<!DOCTYPE html><html lang='pt'><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Configuração Ecowitt</title>";
  html += "<style>body{font-family:-apple-system,sans-serif;background:#f1f5f9;color:#0f172a;max-width:440px;margin:24px auto;padding:0 16px;}";
  html += ".card{background:#fff;padding:22px;border-radius:16px;box-shadow:0 2px 10px rgba(0,0,0,0.06);}";
  html += "h2{margin-top:0;font-size:1.3rem;}label{font-size:0.85rem;font-weight:600;color:#475569;}";
  html += "input{width:100%;padding:10px;margin:6px 0 16px;border:1px solid #cbd5e1;border-radius:8px;box-sizing:border-box;font-size:0.95rem;}";
  html += "button,.back{display:block;text-align:center;width:100%;padding:11px;border-radius:8px;font-size:15px;font-weight:600;cursor:pointer;text-decoration:none;box-sizing:border-box;}";
  html += "button{background:#0284c7;color:#fff;border:none;} .danger{background:#dc2626;margin-top:10px;} .back{background:#e2e8f0;color:#0f172a;margin-top:12px;}</style></head><body>";
  html += "<div class='card'><h2>⚙️ Configuração Ecowitt</h2>";
  html += "<form action='/save' method='POST'>";
  html += "<label>Application Key:</label><input name='appKey' value='" + appKey + "' required>";
  html += "<label>API Key (from Ecowitt Profile):</label><input name='apiKey' value='" + apiKey + "' required>";
  html += "<label>Station MAC Address:</label><input name='macAddr' value='" + macAddr + "' required>";
  html += "<label>Update Interval (minutes):</label><input type='number' name='refresh' min='1' max='120' value='" + String(refreshMins) + "'>";
  html += "<button type='submit'>Guardar & Atualizar Ecrã</button></form>";
  html += "<form action='/resetwifi' method='POST'><button type='submit' class='danger'>Reset Wi-Fi & Abrir AP</button></form>";
  html += "<a href='/' class='back'>← Voltar ao Dashboard</a></div></body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("appKey")) appKey = server.arg("appKey");
  if (server.hasArg("apiKey")) apiKey = server.arg("apiKey");
  if (server.hasArg("macAddr")) macAddr = server.arg("macAddr");
  if (server.hasArg("refresh")) refreshMins = max(1, (int)server.arg("refresh").toInt());

  appKey.trim();
  apiKey.trim();
  macAddr.trim();

  saveConfig();
  forceRefresh = true;
  server.send(200, "text/html", "<h3>Saved! Refreshing E-Paper display...</h3><a href='/'>Back</a>");
}

void handleResetWifi() {
  server.send(200, "text/html", "<h3>Wi-Fi cleared. Rebooting into AP mode (LilyGo-Weather-Setup)...</h3>");
  delay(1000);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

String degToCardinal(const char* degStr) {
  if (!degStr || strlen(degStr) == 0) return "--";
  int deg = atoi(degStr);
  const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int index = (int)((deg + 22.5) / 45.0) % 8;
  return String(dirs[index]);
}



void fetchEcowittData() {
  lastDebugInfo = "fetchEcowittData() started. apiKey len=" + String(apiKey.length());

  if (apiKey.length() < 5) {
    weather.status = "Set API Key at " + WiFi.localIP().toString();
    lastDebugInfo += "\nAborted: apiKey too short.";
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String host = "https://ecowitt.net";
  String path = "/api/v3/device/real_time?application_key=";
  String url = host + path + appKey +
               "&api_key=" + apiKey +
               "&mac=" + macAddr +
               "&call_back=all" +
               "&temp_unitid=1&wind_speed_unitid=7&rainfall_unitid=12&solarradiation_unitid=10&pressure_unitid=3";

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  lastDebugInfo = "Attempting URL:\n" + url + "\n";

  if (!http.begin(client, url)) {
    lastDebugInfo += "\nERROR: http.begin() returned FALSE!";
    weather.status = "HTTP Begin Fail";
    return;
  }

  int httpCode = http.GET();
  String payload = http.getString();
  lastDebugInfo += "\nHTTP Code: " + String(httpCode) + "\n\nPayload:\n" + payload;

  if (httpCode == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);

    if (!err && doc["code"].as<int>() == 0) {
      JsonObject data = doc["data"];

      const char* t = data["outdoor"]["temperature"]["value"];
      const char* h = data["outdoor"]["humidity"]["value"];
      const char* w = data["wind"]["wind_speed"]["value"];
      const char* wd = data["wind"]["wind_direction"]["value"];
      const char* r = data["rainfall"]["daily"]["value"];
      const char* s = data["solar_and_uvi"]["solar"]["value"];
      const char* b = data["battery"]["sensor_array"]["value"];
      const char* fl = data["outdoor"]["feels_like"]["value"];
      const char* dp = data["outdoor"]["dew_point"]["value"];
      const char* wg = data["wind"]["wind_gust"]["value"];
      const char* rr = data["rainfall"]["rain_rate"]["value"];
      const char* uv = data["solar_and_uvi"]["uvi"]["value"];
      const char* pr = data["pressure"]["relative"]["value"];
      const char* it = data["indoor"]["temperature"]["value"];
      const char* ih = data["indoor"]["humidity"]["value"];

      weather.feelsLike  = fl ? String(fl) : "--";
      weather.dewPoint   = dp ? String(dp) : "--";
      weather.windGust   = wg ? String(wg) : "--";
      weather.windDeg    = wd ? String(wd) : "--";
      weather.rainRate   = rr ? String(rr) : "0.0";
      weather.uvi        = uv ? String(uv) : "--";
      weather.pressure   = pr ? String(pr) : "--";
      weather.indoorTemp = it ? String(it) : "--";
      weather.indoorHum  = ih ? String(ih) : "--";
      weather.temp     = t ? String(t) : "--";
      weather.humidity = h ? String(h) : "--";
      weather.wind     = w ? String(w) : "--";
      weather.windDir = degToCardinal(wd);
      weather.rain     = r ? String(r) : "0.0";
      weather.solar    = s ? String(s) : "--";

      if (!b) {
        weather.battery = "--";
      } else if (strcmp(b, "0") == 0) {
        weather.battery = "OK";
      } else if (strcmp(b, "1") == 0) {
        weather.battery = "LOW";
      } else {
        weather.battery = String(b);
      }

      weather.status = WiFi.localIP().toString();

      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 2000)) {
char timeStr[20];
        strftime(timeStr, sizeof(timeStr), "%d/%m %H:%M", &timeinfo);
        weather.lastUpdate = String(timeStr);
        addHistoryPoint(weather.lastUpdate, weather.temp, weather.humidity, weather.pressure);

      } else {
        const char* apiTime = doc["time"];
        if (apiTime) {
          time_t rawTime = (time_t)atol(apiTime);
          struct tm *ptm = localtime(&rawTime);
          if (ptm) {
char timeStr[20];
            strftime(timeStr, sizeof(timeStr), "%d/%m %H:%M", ptm);
            weather.lastUpdate = String(timeStr);
          }
        }
      }
    } else {
      const char* msg = doc["msg"];
      weather.status = "API Err: " + String(msg ? msg : "Parse");
    }
  } else {
    weather.status = "HTTP Err: " + String(httpCode);
  }
  http.end();
}





// Campo do e-paper afetado por cada tipo de aviso: 1=Temp 2=Wind 3=Rain 4=Hum (0 = nenhum)
uint8_t alertField(const String& t) {
  if (t.indexOf("Nevoeiro") >= 0) return 4;
  if (t.indexOf("Precip") >= 0 || t.indexOf("Trovoada") >= 0 || t.indexOf("Neve") >= 0) return 3;
  if (t.indexOf("Vento") >= 0) return 2;
  if (t.indexOf("Quente") >= 0 || t.indexOf("Frio") >= 0) return 1;
  return 0;
}

// Nome curto (ASCII, para a fonte embutida do rodapé)
const char* alertLabel(const String& t) {
  if (t.indexOf("Nevoeiro") >= 0) return "Nevoeiro";
  if (t.indexOf("Precip") >= 0) return "Chuva";
  if (t.indexOf("Trovoada") >= 0) return "Trovoada";
  if (t.indexOf("Neve") >= 0) return "Neve";
  if (t.indexOf("Vento") >= 0) return "Vento";
  if (t.indexOf("Quente") >= 0) return "Calor";
  if (t.indexOf("Frio") >= 0) return "Frio";
  if (t.indexOf("Agita") >= 0) return "Mar";
  return "Outro";
}

// Calcula níveis por campo e texto do rodapé a partir de alerts[]
void computeAlertState() {
  for (int i = 0; i < 5; i++) fieldLevel[i] = 0;
  uint8_t maxL = 0;
  String labels = "";
  for (int i = 0; i < alertCount; i++) {
    if (!alerts[i].active || alerts[i].level < ALERT_MIN_LEVEL) continue;
    if (alerts[i].level > maxL) maxL = alerts[i].level;
    String lb = alertLabel(alerts[i].type);
    if (labels.indexOf(lb) < 0) { if (labels.length()) labels += ","; labels += lb; }
    uint8_t f = alertField(alerts[i].type);
    if (f && alerts[i].level > fieldLevel[f]) fieldLevel[f] = alerts[i].level;
  }
  const char* lvlPt[] = {"", "AMARELO", "LARANJA", "VERMELHO"};
  alertFooter = maxL ? String("AVISO ") + lvlPt[maxL] + ": " + labels : "";
  alertStatus = "OK";
}

// ---- MODO DE TESTE: avisos falsos (sem consultar o IPMA) ----
#define ALERT_TEST_MODE  0      // 1 = usar avisos de teste, 0 = IPMA real
#define ALERT_TEST_LEVEL 2      // 1=amarelo 2=laranja 3=vermelho

void fetchAlerts() {
#if ALERT_TEST_MODE
  alertCount = 1;
  alerts[0].level  = ALERT_TEST_LEVEL;
  alerts[0].active = true;
  alerts[0].type   = "Precipitação";
  alerts[0].start  = "2026-11-23T00:00:00";
  alerts[0].end    = "2026-11-30T23:59:00";
  alerts[0].text   = "AVISO DE TESTE: chuva persistente, por vezes forte.";
  computeAlertState();
  alertStatus = "OK (teste)";
  return;
#endif
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);          // sem "chunked": permite ler o stream em bruto
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  if (!http.begin(client, IPMA_WARN_URL)) { alertStatus = "begin falhou"; return; }
  int code = http.GET();
  if (code != 200) { alertStatus = "HTTP " + String(code); http.end(); return; }

  char nowStr[20] = "";
  bool haveNow = false;
  struct tm ti;
  if (getLocalTime(&ti, 100)) {
    strftime(nowStr, sizeof(nowStr), "%Y-%m-%dT%H:%M:%S", &ti);
    haveNow = true;
  }

  AlertItem tmp[MAX_ALERTS];
  int n = 0, parsed = 0;

  // O ficheiro é grande: lê-se objeto a objeto (sem o carregar todo para a RAM)
  WiFiClient* st = http.getStreamPtr();
  String obj;
  obj.reserve(768);
  int depth = 0;
  bool inStr = false, esc = false, overflow = false;
  unsigned long lastData = millis();

  while ((http.connected() || st->available()) && millis() - lastData < 10000) {
    if (!st->available()) { delay(2); continue; }
    lastData = millis();
    char c = (char)st->read();

    if (depth == 0) {
      if (c == '{') { depth = 1; obj = "{"; overflow = false; inStr = false; esc = false; }
      continue;
    }
    if (!overflow) { if (obj.length() < 2048) obj += c; else overflow = true; }

    if (inStr) {
      if (esc) esc = false;
      else if (c == '\\') esc = true;
      else if (c == '"') inStr = false;
    } else {
      if (c == '"') inStr = true;
      else if (c == '{') depth++;
      else if (c == '}') {
        depth--;
        if (depth == 0 && !overflow) {
          JsonDocument d;
          if (!deserializeJson(d, obj)) {
            parsed++;
            const char* area = d["idAreaAviso"];
            const char* lv   = d["awarenessLevelID"];
            if (area && lv && district.equals(area) && n < MAX_ALERTS) {
              uint8_t level = 0;
              if (strcmp(lv, "yellow") == 0) level = 1;
              else if (strcmp(lv, "orange") == 0) level = 2;
              else if (strcmp(lv, "red") == 0) level = 3;
              const char* st_ = d["startTime"] | "";
              const char* en_ = d["endTime"] | "";
              bool expired = haveNow && en_[0] && strcmp(en_, nowStr) < 0;
              if (level > 0 && !expired) {
                tmp[n].level  = level;
                tmp[n].active = !haveNow || (strcmp(st_, nowStr) <= 0);
                tmp[n].type   = String(d["awarenessTypeName"] | "");
                tmp[n].start  = String(st_);
                tmp[n].end    = String(en_);
                String tx     = String(d["text"] | "");
                if (tx.length() > 300) tx = tx.substring(0, 300);
                tmp[n].text   = tx;
                n++;
              }
            }
          }
        }
      }
    }
  }
  http.end();

  if (parsed == 0) { alertStatus = "formato inesperado"; return; }

  // Commit
  alertCount = n;
  for (int i = 0; i < n; i++) alerts[i] = tmp[i];

  computeAlertState();
}

// Pinta o fundo do campo de preto (modo "negativo") se houver aviso; define a cor do texto
void fieldBox(int x, int y, int w, int h, uint8_t lvl) {
  bool inv = (lvl > 0 && lvl >= ALERT_MIN_LEVEL);
  if (inv) display.fillRect(x, y, w, h, GxEPD_BLACK);
  display.setTextColor(inv ? GxEPD_WHITE : GxEPD_BLACK);
}

void handleSetDistrict() {
  String code = server.arg("code");
  code.trim();
  code.toUpperCase();
  bool ok = (code.length() == 3);
  for (int i = 0; ok && i < 3; i++) if (!isAlpha(code[i])) ok = false;
  if (!ok) { server.send(400, "text/plain", "bad code"); return; }
  district = code;
  saveConfig();
  forceAlerts = true;
  forceRefresh = true;
  server.send(200, "text/plain", "OK");
}

void drawDisplay() {
  display.setRotation(1); // Landscape: 250x122 px
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);

    // Divider lines
    display.drawLine(0, 56, 250, 56, GxEPD_BLACK);
    display.drawLine(125, 0, 125, 104, GxEPD_BLACK);
    display.drawLine(0, 104, 250, 104, GxEPD_BLACK);

    // Top-Left: Outdoor Temperature with "Temp" header
    fieldBox(0, 0, 125, 56, fieldLevel[1]);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(6, 16);
    display.print("Temp");
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(6, 48);
    display.print(weather.temp);
    display.setFont(&FreeSansBold9pt7b);
    display.print(" C");

    // Top-Right: Wind + Cardinal Direction
    fieldBox(126, 0, 124, 56, fieldLevel[2]);
    display.setFont(&FreeSans9pt7b);
    display.setCursor(133, 16);
    display.print("Wind (" + weather.windDir + ")");
    display.setFont(&FreeSansBold18pt7b);
    display.setCursor(133, 48);
    display.print(weather.wind);
    display.setFont(&FreeSansBold9pt7b);
    display.print(" km/h");


    // Bottom-Left: Rain & Humidity
    display.setFont(&FreeSans9pt7b);
    fieldBox(0, 57, 125, 25, fieldLevel[3]);
    display.setCursor(6, 76);
    display.print("Rain: " + weather.rain + " mm");
    fieldBox(0, 82, 125, 22, fieldLevel[4]);
    display.setCursor(6, 97);
    display.print("Hum: " + weather.humidity + "%");

    // Bottom-Right: Solar & Station Battery
    fieldBox(126, 57, 124, 47, 0);
    display.setCursor(133, 76);
    display.print("Sol: " + weather.solar + " W/m2");
    display.setCursor(133, 97);
    display.print("Stn Batt: " + weather.battery);

    // Footer bar: IP + Last Fetch Date/Time
    display.setFont(NULL); // Built-in crisp 6x8 font
    if (alertFooter.length()) {
      // Aviso ativo: barra preta com texto branco (substitui o IP enquanto durar)
      display.fillRect(0, 105, 250, 17, GxEPD_BLACK);
      display.setTextColor(GxEPD_WHITE);
      display.setCursor(6, 110);
      display.print(alertFooter.substring(0, 33));
      String hhmm = weather.lastUpdate.length() >= 5 ? weather.lastUpdate.substring(weather.lastUpdate.length() - 5) : "";
      display.setCursor(215, 110);
      display.print(hhmm);
      display.setTextColor(GxEPD_BLACK);
    } else {
      display.setTextColor(GxEPD_BLACK);
      display.setCursor(6, 110);
      display.print(weather.status + " | " + weather.lastUpdate);
    }

  } while (display.nextPage());
  display.hibernate();
}



void setup() {
  Serial.begin(115200);
  pinMode(EPD_POWER, OUTPUT);
  digitalWrite(EPD_POWER, HIGH);
  pinMode(BTN_CONFIG, INPUT);

  display.init(115200, true, 2, false);
  loadConfig();
  server.on("/", handleRoot);
  server.on("/setup", handleSetup);
  server.on("/api/data", handleApiData);
  server.on("/refresh", []() {
    forceRefresh = true;
    server.send(200, "text/plain", "OK");
  });
  server.on("/save", HTTP_POST, handleSave);
  server.on("/resetwifi", HTTP_POST, handleResetWifi);
  server.on("/setdistrict", handleSetDistrict);

  // Show setup instructions on screen while connecting
  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSansBold9pt7b);
    display.setCursor(10, 35);
    display.print("Connecting to Wi-Fi...");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(10, 65);
    display.print("If first run, connect to AP:");
    display.setCursor(10, 88);
    display.print("LilyGo-Weather-Setup");
  } while (display.nextPage());

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("LilyGo-Weather-Setup")) {
    ESP.restart();
  }
  // Sync local time for Portugal (WET/WEST automatic daylight saving)
  configTzTime("WET0WEST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.nist.gov");
  delay(1500);

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/resetwifi", HTTP_POST, handleResetWifi);
/*
  server.on("/debug", []() {
    server.send(200, "text/plain", lastDebugInfo);
  });
  */
  server.begin();

}

void loop() {
  server.handleClient();

  // Pressing onboard button (GPIO39) triggers an immediate refresh
  if (digitalRead(BTN_CONFIG) == LOW) {
    delay(200);
    forceRefresh = true;
  }

  unsigned long intervalMs = (unsigned long)refreshMins * 60UL * 1000UL;
  if (forceRefresh || (millis() - lastFetch >= intervalMs)) {
    forceRefresh = false;
    lastFetch = millis();
    fetchEcowittData();
    if (forceAlerts || millis() - lastAlertFetch >= ALERT_INTERVAL_MS) {
      forceAlerts = false;
      lastAlertFetch = millis();
      fetchAlerts();
    }
    drawDisplay();
  }
}
