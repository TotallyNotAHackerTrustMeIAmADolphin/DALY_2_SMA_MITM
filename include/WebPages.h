#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#else
// Native unit tests (test_systemconfig) include this to check config_html's
// !!LABEL_/!!IN_<key>!! placeholders against SystemConfig - nothing here needs more
// of Arduino than PROGMEM.
#define PROGMEM
#endif

// Single source of truth for the nav bar shared by every page below - each
// PROGMEM literal splices these in via adjacent string-literal concatenation
// (compile-time, zero runtime cost), so the pages can't drift out of sync.
#define NAV_CSS ".nav { background: #1e1e1e; padding: 10px; border-bottom: 2px solid #333; margin-bottom: 10px; text-align: center; } .nav a { color: #4caf50; text-decoration: none; margin: 0 15px; font-weight: bold; }"
#define NAV_BAR "<div class=\"nav\"><a href=\"/\">DASHBOARD</a> | <a href=\"/config\">CONFIGURATION</a> | <a href=\"/logs\">LOGS</a> | <a href=\"/graphs\">GRAPHS</a></div>"

// The "<!DOCTYPE><head><title><meta viewport>" prefix every page starts
// with; a function-like macro so each page's <title> is still its own.
#define HTML_HEAD(title) "<!DOCTYPE HTML><html><head><title>" title "</title><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"

// config_html/logs_html/graphs_html's body rule, byte-for-byte identical
// (index_html's differs - centered text, a lighter font color - so it keeps its own).
#define BASE_CSS "body { font-family: sans-serif; background: #121212; color: #eee; margin: 0; padding: 0; }"

// logs_html/graphs_html's file-list toolbar styling, identical between the two.
#define FILE_PAGE_CSS ".container { max-width: 900px; margin: auto; background: #1e1e1e; padding: 25px; border-radius: 12px; border: 1px solid #333; } .toolbar { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; margin: 15px 0; } select { font-size: 1em; padding: 6px; background: #000; color: #0f0; border: 1px solid #444; border-radius: 4px; flex: 1; min-width: 180px; } .btn { border: none; padding: 10px 16px; border-radius: 5px; cursor: pointer; font-weight: bold; color: white; background: #0277bd; } .note { color: #ff9800; font-size: 0.85em; margin: 5px 0; }"

// logs_html/graphs_html's toolbar markup; the only difference between the two
// pages is which function Reload calls, so that's the macro's argument.
#define FILE_TOOLBAR(reloadFn) "<div class=\"toolbar\"><select id=\"fileSelect\"></select><button class=\"btn\" onclick=\"" reloadFn "()\">Reload</button><a id=\"downloadLink\" class=\"btn\" style=\"text-decoration:none;\" href=\"#\" download>Download</a></div>"

// Single source of truth for the "fetch a file list, filter it, optionally
// populate a <select>, auto-select the newest" JS shared by index_html's
// loadRecentLog(), logs_html's loadList() and graphs_html's loadList();
// also holds fetchOk(), the fetch-then-throw-on-non-2xx wrapper shared by
// every page's fetch call (#46). Kept to one physical line with no JS
// comments in it: a #define's raw-string value can't span real newlines,
// and a `//` comment would swallow the rest of the macro.
#define SHARED_LIST_JS R"jssrc( async function fetchOk(url, init) { const res = await fetch(url, init); if (!res.ok) throw new Error('HTTP ' + res.status); return res; } async function fetchAndPopulateSelect(url, filterFn, selectEl, statusEl, labelFn) { let files = await (await fetchOk(url)).json(); if (filterFn) files = files.filter(filterFn); if (statusEl) statusEl.innerText = ''; if (selectEl) { selectEl.innerHTML = ''; files.forEach(f => { const opt = document.createElement('option'); opt.value = f.name; opt.text = labelFn ? labelFn(f) : f.name; selectEl.appendChild(opt); }); if (files.length) selectEl.selectedIndex = files.length - 1; } return files; } )jssrc"

// logs_html/graphs_html only: armDownload() is their repeated "bail if
// nothing's selected, else point #downloadLink at it" guard; initFilePage()
// replaces their structurally-identical loadList(), differing only in options.
#define FILE_PAGE_JS R"jssrc( function armDownload(sel) { if (!sel.value) return false; document.getElementById('downloadLink').href = '/api/logs/download?file=' + encodeURIComponent(sel.value); return true; } async function initFilePage(o) { const sel = document.getElementById('fileSelect'); const status = document.getElementById('status'); try { const files = await fetchAndPopulateSelect('/api/logs/list', o.filterFn, sel, status, o.labelFn); if (!files.length) { o.emptyEl().innerText = o.emptyMsg; return; } o.onLoaded(); } catch (e) { status.innerText = 'Failed to list log files: ' + e.message; } } )jssrc"

const char index_html[] PROGMEM = HTML_HEAD("BMS Bridge Pro") R"rawliteral(
<style>
  body { font-family: sans-serif; text-align: center; background: #121212; color: #e0e0e0; margin: 0; }
  )rawliteral" NAV_CSS R"rawliteral(
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 10px; padding: 15px; }
  .card { background: #1e1e1e; padding: 15px; border-radius: 10px; border: 1px solid #333; }
  .value { font-size: 2em; font-weight: bold; color: #4caf50; }
  .negative-val { color: #f44336 !important; }
  .err-active { color: #f44336 !important; }
  .btn { border: none; padding: 12px 20px; border-radius: 5px; cursor: pointer; font-weight: bold; margin: 10px; display: inline-block; text-decoration: none; color: white; }
  .btn-blue { background: #0277bd; } .btn-red { background: #d32f2f; }
  
  /* Cell Grid Styles */
  .cells-container { background: #1e1e1e; padding: 15px; border-radius: 10px; border: 1px solid #333; margin: 0 15px 15px 15px; }
  .cells-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(65px, 1fr)); gap: 8px; margin-top: 15px; }
  .cell-box { background: #2a2a2a; padding: 10px 5px; border-radius: 5px; font-size: 0.85em; border: 1px solid #444; color: #aaa; position: relative; overflow: hidden; }
  .cell-box span { display: block; font-size: 1.25em; font-weight: bold; margin-top: 4px; color: #e0e0e0; }
  .cell-fill { position: absolute; bottom: 0; left: 0; right: 0; background: rgba(76, 175, 80, 0.4); transition: height 0.3s; z-index: 1; }
  .cell-content { position: relative; z-index: 2; }
  .cell-min { border-color: #2196F3; background: rgba(33, 150, 243, 0.1); }
  .cell-min span { color: #2196F3; }
  .cell-max { border-color: #f44336; background: rgba(244, 67, 54, 0.1); }
  .cell-max span { color: #f44336; }
  
  #console { box-sizing: border-box; width: 95%; max-width: 1000px; height: 300px; margin: 15px auto; background: #000; color: #00ff00; font-family: monospace; text-align: left; padding: 15px; overflow-y: scroll; border-radius: 8px; border: 1px solid #444; }
</style></head><body>
)rawliteral" NAV_BAR R"rawliteral(
<div class="grid">
  <div class="card"><div>Pack Voltage</div><div id="v" class="value">--</div></div>
  <div class="card"><div>Charge Limit (CCL)</div><div id="reqI" class="value">--</div></div>
  <div class="card"><div>Cell Spread</div><div id="spread" class="value">--</div></div>
  <div class="card"><div>Current</div><div id="i" class="value">--</div></div>
  <div class="card"><div>SOC</div><div id="soc" class="value">--</div></div>
  <div class="card"><div>SMA Status</div><div id="smastat" class="value">--</div></div>
  <div class="card"><div>Max Cell V</div><div id="maxCellV" class="value">--</div></div>
  <div class="card"><div>Min Cell V</div><div id="minCellV" class="value">--</div></div>
</div>

<div class="cells-container">
  <div style="font-weight: bold; color: #4caf50; font-size: 1.2em;">Individual Cell Voltages</div>
  <div class="cells-grid" id="cGrid"><div style="grid-column: 1 / -1;">Waiting for BMS data...</div></div>
</div>

<div class="card" style="margin: 0 15px;">
  <button class="btn btn-blue" id="mbtn" onclick="postAction('/toggleMaint')">TRIGGER FORCE CHARGE</button>
  <button class="btn btn-red" onclick="if(confirm('Simulate battery disconnect?')) postAction('/resetSMA')">CLEAR SMA ERROR (Reset)</button>
</div>
<div id="console">Loading history...<br></div>
<script>
)rawliteral" SHARED_LIST_JS R"rawliteral(
  const con = document.getElementById('console');
  // One <div> per line, text only (a log line is never markup), capped at
  // CON_MAX_LINES. The old innerHTML += line + "<br>" added two nodes per
  // line but removed one, so the console grew without bound (#68).
  const CON_MAX_LINES = 100;
  function conAppend(text) {
    const line = document.createElement('div');
    line.textContent = text;
    con.appendChild(line);
    while (con.childNodes.length > CON_MAX_LINES) con.removeChild(con.firstChild);
  }
  function conReset(text) { con.textContent = ''; conAppend(text); }

  // POSTs a UI action (toggleMaint/resetSMA); a 503 means the device held
  // dataMutex too long to apply it, so the caller can just try again.
  async function postAction(url) {
    try { await fetchOk(url, { method: 'POST' }); }
    catch (e) { alert('Action failed: ' + e.message); }
  }

  // Seed the console with the tail of today's SD .log file on load, so it
  // shows recent history instead of only events that happen to fire after
  // this tab connects (the SSE 'log' channel has no replay/backlog).
  async function loadRecentLog() {
    try {
      const files = await fetchAndPopulateSelect('/api/logs/list', f => f.name.endsWith('.log'), null, null);
      if (!files.length) { conReset('Log Active... (no SD log file yet)'); return; }
      const latest = files[files.length - 1].name; // listLogFiles sorts ascending -> last = newest
      const res = await fetchOk('/api/logs/content?file=' + encodeURIComponent(latest));
      const lines = (await res.text()).split('\n').filter(l => l.length > 0);
      con.textContent = '';
      lines.forEach(conAppend);
      conAppend('--- live ---');
      con.scrollTop = con.scrollHeight;
    } catch (e) {
      conReset('Log Active... (failed to load SD history: ' + e.message + ')');
    }
  }
  loadRecentLog();

  var source = new EventSource('/events');
  source.addEventListener('data', function(e) {
    var obj = JSON.parse(e.data);
    document.getElementById('v').innerHTML = obj.v.toFixed(2) + " V";
    document.getElementById('reqI').innerHTML = obj.reqI.toFixed(1) + " A";
    // maxC/minC are the smoothed (~48s moving average) values. The raw
    // single-read values (maxCellRaw/minCellRaw, which the hard cutoff and
    // alarm gate act on - #9) stay in the SSE JSON and the CSV (MinCellRaw/
    // MaxCellRaw) for the Graphs/Logs pages; the dashboard shows smoothed
    // values only, like the cell grid below.
    document.getElementById('maxCellV').innerHTML = obj.maxC.toFixed(3) + " V";
    document.getElementById('minCellV').innerHTML = obj.minC.toFixed(3) + " V";

    // Cell spread (#24) - raw max-min cell voltage and the resulting
    // current-limit derating factor (1.0 = no derating).
    const spreadEl = document.getElementById('spread');
    const deratePct = Math.round((1 - obj.derate) * 100);
    spreadEl.innerHTML = obj.spreadMv + " mV (" + (deratePct > 0 ? "derating " + deratePct + "%" : "none") + ")";
    if (deratePct > 0) spreadEl.classList.add('negative-val');
    else spreadEl.classList.remove('negative-val');

    const curEl = document.getElementById('i');
    curEl.innerHTML = obj.i.toFixed(1) + " A";
    if (obj.i < -0.1) curEl.classList.add('negative-val');
    else curEl.classList.remove('negative-val');
    document.getElementById('soc').innerHTML = obj.soc.toFixed(1) + "%";
    
    // --- SMA MODE DISPLAY ---
    document.getElementById('smastat').innerHTML = obj.smam.toUpperCase();
    document.getElementById('smastat').className = "value";
    
    const mb = document.getElementById('mbtn');
    if(obj.force) { mb.innerHTML = "STOP FORCE CHARGE"; mb.style.background = "#ff9800"; }
    else { mb.innerHTML = "TRIGGER FORCE CHARGE"; mb.style.background = "#0277bd"; }

    // Render 16 cells
    if(obj.cells && obj.cells.length > 0) {
      var cg = document.getElementById('cGrid');
      var html = "";
      var range = obj.maxC - obj.minC;
      for(var i=0; i<obj.cells.length; i++) {
        var cls = "cell-box";
        var pct = range > 0.001 ? ((obj.cells[i] - obj.minC) / range) * 100 : 100;
        if(obj.cells[i] === obj.minC) cls += " cell-min";
        else if(obj.cells[i] === obj.maxC) cls += " cell-max";
        html += "<div class='"+cls+"'><div class='cell-fill' style='height:"+pct+"%'></div><div class='cell-content'>C"+(i+1)+"<span>"+obj.cells[i].toFixed(3)+"V</span></div></div>";
      }
      cg.innerHTML = html;
    }
  }, false);
  source.addEventListener('log', function(e) {
    conAppend(e.data);
    con.scrollTop = con.scrollHeight;
  }, false);
</script></body></html>)rawliteral";

const char config_html[] PROGMEM = HTML_HEAD("Settings") R"rawliteral(
<style>
  )rawliteral" BASE_CSS NAV_CSS R"rawliteral(
  .container { max-width: 650px; margin: auto; background: #1e1e1e; padding: 25px; border-radius: 12px; border: 1px solid #333; }
  .row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; border-bottom: 1px solid #2a2a2a; padding-bottom: 8px; }
  .text-group { text-align: left; padding-right: 15px; }
  .field { display: flex; flex-direction: column; align-items: flex-end; flex-shrink: 0; }
  .range { font-size: 0.7em; color: #666; margin-top: 3px; white-space: nowrap; }
  .desc { font-size: 0.8em; color: #888; display: block; margin-top: 2px; }
  .hint { font-size: 0.75em; color: #666; display: block; margin-top: 2px; font-style: italic; }
  h2 { color: #4caf50; border-bottom: 2px solid #4caf50; padding-bottom: 5px; margin-top: 25px; }
  .winter-h { color: #ff9800 !important; border-bottom: 2px solid #ff9800 !important; }
  input { font-size: 1.1em; padding: 5px; width: 110px; text-align: center; background: #000; color: #0f0; border: 1px solid #444; border-radius: 4px; }
  .save { background: #2e7d32; color: white; border: none; padding: 15px; width: 100%; border-radius: 5px; font-weight: bold; cursor: pointer; font-size: 1.1em; margin-top: 20px; }
</style></head><body>
)rawliteral" NAV_BAR R"rawliteral(
<div class="container">
  <form action="/save" method="GET">
    <h2>Charging Profile (16S)</h2>
    <div class="row"><div class="text-group"><strong>!!LABEL_ca!!</strong><span class="desc">Global bulk charging limit.</span></div>
      !!IN_ca!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_cvt!!</strong><span class="desc">Current begins to slow at this cell voltage.</span></div>
      !!IN_cvt!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_cag!!</strong><span class="desc">Voltage where balancing floor is reached.</span></div>
      !!IN_cag!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_ta!!</strong><span class="desc">Constant current floor for balancing.</span></div>
      !!IN_ta!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_cmv!!</strong><span class="desc">Absolute cell safety cutoff (Hard Floor).</span><span class="hint">max = Daly OV protection minus a 100 mV margin (#8)</span></div>
      !!IN_cmv!!</div>

    <h2 class="winter-h">Winter Force Charge</h2>
    <div class="row"><div class="text-group"><strong>!!LABEL_cmsv!!</strong><span class="desc">Trigger grid charge if any cell (smoothed) falls below this. Must be above Min Discharge Vpc, or the top-up can only start after discharge is already cut.</span></div>
      !!IN_cmsv!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_cmpp!!</strong><span class="desc">Stop grid charge when cells reach this.</span></div>
      !!IN_cmpp!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_mam!!</strong><span class="desc">Constant current drawn from grid.</span></div>
      !!IN_mam!!</div>

    <h2>Discharging Profile</h2>
    <div class="row"><div class="text-group"><strong>!!LABEL_da!!</strong><span class="desc">Peak household load limit.</span></div>
      !!IN_da!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_cdvt!!</strong><span class="desc">Voltage where discharge current is restricted.</span></div>
      !!IN_cdvt!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_clag!!</strong><span class="desc">Entry point for keeping-alive mode.</span></div>
      !!IN_clag!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_ld_v2!!</strong><span class="desc">Minimum keeping-alive current floor.</span></div>
      !!IN_ld_v2!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_cmdv!!</strong><span class="desc">Absolute floor to prevent cell reversal.</span></div>
      !!IN_cmdv!!</div>

    <h2>System Tuning</h2>
    <div class="row"><div class="text-group"><strong>!!LABEL_vs!!</strong><span class="desc">Number of moving average samples.</span></div>
      !!IN_vs!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_to!!</strong><span class="desc">Seconds without a complete BMS read (basic info and cell voltages) before the charge and discharge limits drop to 0 A. Counts from the older of the two reads.</span><span class="hint">typical 30&ndash;120</span></div>
      !!IN_to!!</div>

    <h2>Cell Spread Derating (#24)</h2>
    <div class="row"><div class="text-group"><strong>!!LABEL_sps!!</strong><span class="desc">Below this spread the current limits are untouched. Above it they are reduced linearly, reaching trickle/limp current at the 'full derating' value below. Spread = highest raw cell voltage minus lowest.</span><span class="hint">typical 40&ndash;100</span></div>
      !!IN_sps!!</div>
    <div class="row"><div class="text-group"><strong>!!LABEL_spm!!</strong><span class="desc">At or above this spread the current limits are held at trickle/limp current, same floor as the voltage alarm gate. Below it, derating eases back off toward the 'start derating' value above. Spread = highest raw cell voltage minus lowest.</span><span class="hint">typical 120&ndash;200</span></div>
      !!IN_spm!!</div>

    <button type="submit" class="save">SAVE & APPLY ALL CHANGES</button>
  </form>
</div></body></html>)rawliteral";

const char logs_html[] PROGMEM = HTML_HEAD("Logs") R"rawliteral(
<style>
  )rawliteral" BASE_CSS NAV_CSS FILE_PAGE_CSS R"rawliteral(
  #content { background: #000; color: #0f0; font-family: monospace; font-size: 0.85em; white-space: pre-wrap; word-break: break-all; padding: 15px; border-radius: 8px; border: 1px solid #444; height: 500px; overflow-y: scroll; }
</style></head><body>
)rawliteral" NAV_BAR R"rawliteral(
<div class="container">
  <h2 style="color:#4caf50;">SD Card Logs</h2>
  )rawliteral" FILE_TOOLBAR("loadFile") R"rawliteral(
  <div id="status" class="note"></div>
  <div id="content">Loading file list...</div>
</div>
<script>
)rawliteral" SHARED_LIST_JS FILE_PAGE_JS R"rawliteral(
  async function loadFile() {
    const sel = document.getElementById('fileSelect');
    const content = document.getElementById('content');
    const status = document.getElementById('status');
    if (!armDownload(sel)) return;
    status.innerText = '';
    content.innerText = 'Loading...';
    try {
      const res = await fetchOk('/api/logs/content?file=' + encodeURIComponent(sel.value));
      const text = await res.text();
      content.innerText = text;
      content.scrollTop = content.scrollHeight;
      if (res.headers.get('X-Truncated') === '1') {
        status.innerText = 'Showing only the most recent portion of this file (it is larger than the view limit). Use Download for the full file.';
      }
    } catch (e) {
      status.innerText = 'Failed to load file: ' + e.message;
    }
  }

  initFilePage({
    filterFn: null,
    labelFn: f => f.name + ' (' + Math.round(f.size / 1024) + ' KB)',
    emptyEl: () => document.getElementById('content'),
    emptyMsg: 'No log files found (SD card missing or empty).',
    onLoaded: loadFile
  });
</script></body></html>)rawliteral";

const char graphs_html[] PROGMEM = HTML_HEAD("Graphs") R"rawliteral(
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
  )rawliteral" BASE_CSS NAV_CSS FILE_PAGE_CSS R"rawliteral(
  .chart-box { background: #1a1a1a; border: 1px solid #333; border-radius: 8px; padding: 10px; margin: 15px 0; }
  h3 { color: #4caf50; margin: 5px 0 10px 0; font-size: 1em; }
</style></head><body>
)rawliteral" NAV_BAR R"rawliteral(
<div class="container">
  <h2 style="color:#4caf50;">Trend Graphs</h2>
  )rawliteral" FILE_TOOLBAR("loadGraph") R"rawliteral(
  <div id="status" class="note"></div>

  <div class="chart-box"><h3>Pack Voltage</h3><canvas id="chartV"></canvas></div>
  <div class="chart-box"><h3>State of Charge</h3><canvas id="chartSoc"></canvas></div>
  <div class="chart-box"><h3>Pack Current &amp; Requested Current</h3><canvas id="chartI"></canvas></div>
  <div class="chart-box"><h3>Min / Max Cell Voltage</h3><canvas id="chartCell"></canvas></div>
</div>
<script>
)rawliteral" SHARED_LIST_JS FILE_PAGE_JS R"rawliteral(
  let charts = {};

  function darkChart(canvasId, labels, datasets, extraScales) {
    const ctx = document.getElementById(canvasId).getContext('2d');
    if (charts[canvasId]) charts[canvasId].destroy();
    charts[canvasId] = new Chart(ctx, {
      type: 'line',
      data: { labels: labels, datasets: datasets },
      options: {
        animation: false,
        interaction: { mode: 'index', intersect: false },
        scales: Object.assign({
          x: { ticks: { color: '#888', maxTicksLimit: 12 }, grid: { color: '#222' } }
        }, extraScales),
        // A single series needs no legend box - the chart title already says
        // what's plotted; only show it once there's more than one series.
        plugins: { legend: { display: datasets.length > 1, labels: { color: '#ccc' } } }
      }
    });
    return charts[canvasId];
  }

  function parseCSV(text) {
    const lines = text.trim().split('\n');
    const header = lines.shift().split(',');
    const col = {};
    header.forEach((name, i) => { col[name] = i; });
    const labels = [], packV = [], soc = [], packI = [], reqI = [], minC = [], maxC = [];
    lines.forEach(line => {
      const c = line.split(',');
      if (c.length < header.length) return;
      const t = c[col.Timestamp];
      labels.push(t.includes(' ') ? t.split(' ')[1] : t);
      packV.push(parseFloat(c[col.PackV]));
      packI.push(parseFloat(c[col.PackI]));
      soc.push(parseFloat(c[col.SOC]));
      minC.push(parseFloat(c[col.MinCellV]));
      maxC.push(parseFloat(c[col.MaxCellV]));
      reqI.push(parseFloat(c[col.ReqI]));
    });
    return { labels, packV, soc, packI, reqI, minC, maxC };
  }

  async function loadGraph() {
    const sel = document.getElementById('fileSelect');
    const status = document.getElementById('status');
    if (!armDownload(sel)) return;
    status.innerText = 'Loading...';
    try {
      const res = await fetchOk('/api/logs/graph?file=' + encodeURIComponent(sel.value));
      const data = parseCSV(await res.text());
      status.innerText = data.labels.length + ' points shown (downsampled for display).';

      darkChart('chartV', data.labels, [
        { label: 'Pack V', data: data.packV, borderColor: '#4caf50', yAxisID: 'yV', pointRadius: 0 }
      ], {
        yV: { position: 'left', ticks: { color: '#4caf50' }, grid: { color: '#222' } }
      });

      darkChart('chartSoc', data.labels, [
        { label: 'SOC %', data: data.soc, borderColor: '#ff9800', yAxisID: 'ySoc', pointRadius: 0 }
      ], {
        ySoc: { position: 'left', min: 0, max: 100, ticks: { color: '#ff9800' }, grid: { color: '#222' } }
      });

      darkChart('chartI', data.labels, [
        { label: 'Pack Current (A)', data: data.packI, borderColor: '#2196F3', yAxisID: 'yI', pointRadius: 0 },
        { label: 'Requested Current (A)', data: data.reqI, borderColor: '#9c27b0', yAxisID: 'yI', pointRadius: 0 }
      ], {
        yI: { position: 'left', ticks: { color: '#ccc' }, grid: { color: '#222' } }
      });

      darkChart('chartCell', data.labels, [
        { label: 'Min Cell V', data: data.minC, borderColor: '#2196F3', yAxisID: 'yC', pointRadius: 0 },
        { label: 'Max Cell V', data: data.maxC, borderColor: '#f44336', yAxisID: 'yC', pointRadius: 0 }
      ], {
        yC: { position: 'left', ticks: { color: '#ccc' }, grid: { color: '#222' } }
      });
    } catch (e) {
      status.innerText = 'Failed to load graph: ' + e.message;
    }
  }

  initFilePage({
    filterFn: f => f.name.endsWith('.csv'),
    labelFn: undefined,
    emptyEl: () => document.getElementById('status'),
    emptyMsg: 'No telemetry CSV files found (SD card missing or empty).',
    onLoaded: loadGraph
  });
</script></body></html>)rawliteral";
