#pragma once
#include <Arduino.h>

// Single source of truth for the nav bar shared by every page below - each
// PROGMEM literal splices these in via adjacent string-literal concatenation
// (a compile-time, zero-runtime-cost operation), so a page can never drift
// out of sync with the others the way the standalone "Back to Dashboard"
// links and the once-forgotten body margin:0 did.
#define NAV_CSS ".nav { background: #1e1e1e; padding: 10px; border-bottom: 2px solid #333; margin-bottom: 10px; text-align: center; } .nav a { color: #4caf50; text-decoration: none; margin: 0 15px; font-weight: bold; }"
#define NAV_BAR "<div class=\"nav\"><a href=\"/\">DASHBOARD</a> | <a href=\"/config\">CONFIGURATION</a> | <a href=\"/logs\">LOGS</a> | <a href=\"/graphs\">GRAPHS</a></div>"

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>BMS Bridge Pro</title><meta name="viewport" content="width=device-width, initial-scale=1">
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
  
  #console { width: 95%; max-width: 1000px; height: 300px; margin: 15px auto; background: #000; color: #00ff00; font-family: monospace; text-align: left; padding: 15px; overflow-y: scroll; border-radius: 8px; border: 1px solid #444; }
</style></head><body>
)rawliteral" NAV_BAR R"rawliteral(
<div class="grid">
  <div class="card"><div>Pack Voltage</div><div id="v" class="value">--</div></div>
  <div class="card"><div>Req. Current</div><div id="reqI" class="value">--</div></div>
  <div class="card"><div>Delta (Max-Min)</div><div id="cv" class="value">--</div></div>
  <div class="card"><div>Current</div><div id="i" class="value">--</div></div>
  <div class="card"><div>SOC</div><div id="soc" class="value">--</div></div>
  <div class="card"><div>SMA Status</div><div id="smastat" class="value">--</div></div>
</div>

<div class="cells-container">
  <div style="font-weight: bold; color: #4caf50; font-size: 1.2em;">Individual Cell Voltages</div>
  <div class="cells-grid" id="cGrid"><div style="grid-column: 1 / -1;">Waiting for BMS data...</div></div>
</div>

<div class="card" style="margin: 0 15px;">
  <a href="/toggleMaint" class="btn btn-blue" id="mbtn">TRIGGER FORCE CHARGE</a>
  <button class="btn btn-red" onclick="if(confirm('Simulate battery disconnect?')) fetch('/resetSMA')">CLEAR SMA ERROR (Reset)</button>
</div>
<div id="console">Loading history...<br></div>
<script>
  const con = document.getElementById('console');

  // Seed the console with the tail of today's SD .log file on load, so it
  // shows recent history instead of only events that happen to fire after
  // this tab connects (the SSE 'log' channel has no replay/backlog).
  async function loadRecentLog() {
    try {
      const files = (await (await fetch('/api/logs/list')).json()).filter(f => f.name.endsWith('.log'));
      if (!files.length) { con.innerHTML = 'Log Active... (no SD log file yet)<br>'; return; }
      const latest = files[files.length - 1].name; // listLogFiles sorts ascending -> last = newest
      const text = await (await fetch('/api/logs/content?file=' + encodeURIComponent(latest))).text();
      const lines = text.split('\n').filter(l => l.length > 0).slice(-100);
      con.innerHTML = (lines.length ? lines.join('<br>') + '<br>' : '') + '--- live ---<br>';
      con.scrollTop = con.scrollHeight;
    } catch (e) {
      con.innerHTML = 'Log Active... (failed to load SD history: ' + e.message + ')<br>';
    }
  }
  loadRecentLog();

  var source = new EventSource('/events');
  source.addEventListener('data', function(e) {
    var obj = JSON.parse(e.data);
    document.getElementById('v').innerHTML = obj.v.toFixed(2) + " V";
    document.getElementById('reqI').innerHTML = obj.reqI.toFixed(1) + " A";
    document.getElementById('cv').innerHTML = ((obj.maxC - obj.minC) * 1000).toFixed(0) + " mV";
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
    con.innerHTML += e.data + "<br>";
    if(con.childNodes.length > 100) con.removeChild(con.firstChild);
    con.scrollTop = con.scrollHeight;
  }, false);
</script></body></html>)rawliteral";

const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>Settings</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; background: #121212; color: #eee; margin: 0; padding: 0; }
  )rawliteral" NAV_CSS R"rawliteral(
  .container { max-width: 650px; margin: auto; background: #1e1e1e; padding: 25px; border-radius: 12px; border: 1px solid #333; }
  .row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; border-bottom: 1px solid #2a2a2a; padding-bottom: 8px; }
  .text-group { text-align: left; padding-right: 15px; }
  .desc { font-size: 0.8em; color: #888; display: block; margin-top: 2px; }
  h2 { color: #4caf50; border-bottom: 2px solid #4caf50; padding-bottom: 5px; margin-top: 25px; }
  .winter-h { color: #ff9800 !important; border-bottom: 2px solid #ff9800 !important; }
  input { font-size: 1.1em; padding: 5px; width: 110px; text-align: center; background: #000; color: #0f0; border: 1px solid #444; border-radius: 4px; }
  .save { background: #2e7d32; color: white; border: none; padding: 15px; width: 100%; border-radius: 5px; font-weight: bold; cursor: pointer; font-size: 1.1em; margin-top: 20px; }
</style></head><body>
)rawliteral" NAV_BAR R"rawliteral(
<div class="container">
  <form action="/save" method="GET">
    <h2>Charging Profile (16S)</h2>
    <div class="row"><div class="text-group"><strong>Max Charge Amps</strong><span class="desc">Global bulk charging limit.</span></div>
      <input type="number" name="ca" step="5" value="!!VAL_CA!!"></div>
    <div class="row"><div class="text-group"><strong>Start Taper Vpc</strong><span class="desc">Current begins to slow at this cell voltage.</span></div>
      <input type="number" name="cvt" step="0.001" value="!!VAL_VT!!"></div>
    <div class="row"><div class="text-group"><strong>Target Trickle Vpc</strong><span class="desc">Voltage where balancing floor is reached.</span></div>
      <input type="number" name="cag" step="0.001" value="!!VAL_AG!!"></div>
    <div class="row"><div class="text-group"><strong>Trickle Amps</strong><span class="desc">Constant current floor for balancing.</span></div>
      <input type="number" name="ta" step="0.5" value="!!VAL_TA!!"></div>
    <div class="row"><div class="text-group"><strong>Max Charge Vpc</strong><span class="desc">Absolute cell safety cutoff (Hard Floor).</span></div>
      <input type="number" name="cmv" step="0.001" value="!!VAL_MV!!"></div>

    <h2 class="winter-h">Winter Force Charge</h2>
    <div class="row"><div class="text-group"><strong>Maint. Start Vpc</strong><span class="desc">Trigger grid charge if any cell falls below this.</span></div>
      <input type="number" name="cmsv" step="0.001" value="!!VAL_MSV!!"></div>
    <div class="row"><div class="text-group"><strong>Maint. Stop Vpc</strong><span class="desc">Stop grid charge when cells reach this.</span></div>
      <input type="number" name="cmpp" step="0.001" value="!!VAL_MPP!!"></div>
    <div class="row"><div class="text-group"><strong>Maintenance Amps</strong><span class="desc">Constant current drawn from grid.</span></div>
      <input type="number" name="mam" step="1" value="!!VAL_MAM!!"></div>

    <h2>Discharging Profile</h2>
    <div class="row"><div class="text-group"><strong>Max Discharge Amps</strong><span class="desc">Peak household load limit.</span></div>
      <input type="number" name="da" step="10" value="!!VAL_DA!!"></div>
    <div class="row"><div class="text-group"><strong>Start Taper Vpc (D)</strong><span class="desc">Voltage where discharge current is restricted.</span></div>
      <input type="number" name="cdvt" step="0.001" value="!!VAL_DVT!!"></div>
    <div class="row"><div class="text-group"><strong>Target Limp Vpc</strong><span class="desc">Entry point for keeping-alive mode.</span></div>
      <input type="number" name="clag" step="0.001" value="!!VAL_LAG!!"></div>
    <div class="row"><div class="text-group"><strong>Limp Amps</strong><span class="desc">Minimum keeping-alive current floor.</span></div>
      <input type="number" name="ld_v2" step="1" value="!!VAL_LIMP!!"></div>
    <div class="row"><div class="text-group"><strong>Min Discharge Vpc</strong><span class="desc">Absolute floor to prevent cell reversal.</span></div>
      <input type="number" name="cmdv" step="0.001" value="!!VAL_MDV!!"></div>

    <h2>System Tuning</h2>
    <div class="row"><div class="text-group"><strong>Voltage Window</strong><span class="desc">Number of moving average samples (1-20).</span></div>
      <input type="number" name="vs" step="1" value="!!VAL_VS!!"></div>

    <button type="submit" class="save">SAVE & APPLY ALL CHANGES</button>
  </form>
</div></body></html>)rawliteral";

const char logs_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>Logs</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; background: #121212; color: #eee; margin: 0; padding: 0; }
  )rawliteral" NAV_CSS R"rawliteral(
  .container { max-width: 900px; margin: auto; background: #1e1e1e; padding: 25px; border-radius: 12px; border: 1px solid #333; }
  .toolbar { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; margin: 15px 0; }
  select { font-size: 1em; padding: 6px; background: #000; color: #0f0; border: 1px solid #444; border-radius: 4px; flex: 1; min-width: 180px; }
  .btn { border: none; padding: 10px 16px; border-radius: 5px; cursor: pointer; font-weight: bold; color: white; background: #0277bd; }
  .note { color: #ff9800; font-size: 0.85em; margin: 5px 0; }
  #content { background: #000; color: #0f0; font-family: monospace; font-size: 0.85em; white-space: pre-wrap; word-break: break-all; padding: 15px; border-radius: 8px; border: 1px solid #444; height: 500px; overflow-y: scroll; }
</style></head><body>
)rawliteral" NAV_BAR R"rawliteral(
<div class="container">
  <h2 style="color:#4caf50;">SD Card Logs</h2>
  <div class="toolbar">
    <select id="fileSelect"></select>
    <button class="btn" onclick="loadFile()">Reload</button>
    <a id="downloadLink" class="btn" style="text-decoration:none;" href="#" download>Download</a>
  </div>
  <div id="status" class="note"></div>
  <div id="content">Loading file list...</div>
</div>
<script>
  async function loadList() {
    const sel = document.getElementById('fileSelect');
    const status = document.getElementById('status');
    try {
      const res = await fetch('/api/logs/list');
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const files = await res.json();
      sel.innerHTML = '';
      if (!files.length) {
        document.getElementById('content').innerText = 'No log files found (SD card missing or empty).';
        return;
      }
      files.forEach(f => {
        const opt = document.createElement('option');
        opt.value = f.name;
        opt.text = f.name + ' (' + Math.round(f.size / 1024) + ' KB)';
        sel.appendChild(opt);
      });
      sel.selectedIndex = files.length - 1; // most recent
      loadFile();
    } catch (e) {
      status.innerText = 'Failed to list log files: ' + e.message;
    }
  }

  async function loadFile() {
    const sel = document.getElementById('fileSelect');
    const content = document.getElementById('content');
    const status = document.getElementById('status');
    if (!sel.value) return;
    document.getElementById('downloadLink').href = '/api/logs/download?file=' + encodeURIComponent(sel.value);
    status.innerText = '';
    content.innerText = 'Loading...';
    try {
      const res = await fetch('/api/logs/content?file=' + encodeURIComponent(sel.value));
      if (!res.ok) throw new Error('HTTP ' + res.status);
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

  loadList();
</script></body></html>)rawliteral";

const char graphs_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>Graphs</title><meta name="viewport" content="width=device-width, initial-scale=1">
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
  body { font-family: sans-serif; background: #121212; color: #eee; margin: 0; padding: 0; }
  )rawliteral" NAV_CSS R"rawliteral(
  .container { max-width: 900px; margin: auto; background: #1e1e1e; padding: 25px; border-radius: 12px; border: 1px solid #333; }
  .toolbar { display: flex; gap: 10px; align-items: center; flex-wrap: wrap; margin: 15px 0; }
  select { font-size: 1em; padding: 6px; background: #000; color: #0f0; border: 1px solid #444; border-radius: 4px; flex: 1; min-width: 180px; }
  .btn { border: none; padding: 10px 16px; border-radius: 5px; cursor: pointer; font-weight: bold; color: white; background: #0277bd; }
  .note { color: #ff9800; font-size: 0.85em; margin: 5px 0; }
  .chart-box { background: #1a1a1a; border: 1px solid #333; border-radius: 8px; padding: 10px; margin: 15px 0; }
  h3 { color: #4caf50; margin: 5px 0 10px 0; font-size: 1em; }
</style></head><body>
)rawliteral" NAV_BAR R"rawliteral(
<div class="container">
  <h2 style="color:#4caf50;">Trend Graphs</h2>
  <div class="toolbar">
    <select id="fileSelect"></select>
    <button class="btn" onclick="loadGraph()">Reload</button>
    <a id="downloadLink" class="btn" style="text-decoration:none;" href="#" download>Download</a>
  </div>
  <div id="status" class="note"></div>

  <div class="chart-box"><h3>Pack Voltage</h3><canvas id="chartV"></canvas></div>
  <div class="chart-box"><h3>State of Charge</h3><canvas id="chartSoc"></canvas></div>
  <div class="chart-box"><h3>Pack Current &amp; Requested Current</h3><canvas id="chartI"></canvas></div>
  <div class="chart-box"><h3>Min / Max Cell Voltage</h3><canvas id="chartCell"></canvas></div>
</div>
<script>
  let charts = {};

  function darkChart(canvasId, datasets, extraScales) {
    const ctx = document.getElementById(canvasId).getContext('2d');
    if (charts[canvasId]) charts[canvasId].destroy();
    charts[canvasId] = new Chart(ctx, {
      type: 'line',
      data: { labels: [], datasets: datasets },
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
    lines.shift(); // header
    const labels = [], packV = [], soc = [], packI = [], reqI = [], minC = [], maxC = [];
    lines.forEach(line => {
      const c = line.split(',');
      if (c.length < 7) return;
      const t = c[0];
      labels.push(t.includes(' ') ? t.split(' ')[1] : t);
      packV.push(parseFloat(c[1]));
      packI.push(parseFloat(c[2]));
      soc.push(parseFloat(c[3]));
      minC.push(parseFloat(c[4]));
      maxC.push(parseFloat(c[5]));
      reqI.push(parseFloat(c[6]));
    });
    return { labels, packV, soc, packI, reqI, minC, maxC };
  }

  async function loadList() {
    const sel = document.getElementById('fileSelect');
    const status = document.getElementById('status');
    try {
      const res = await fetch('/api/logs/list');
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const files = (await res.json()).filter(f => f.name.endsWith('.csv'));
      sel.innerHTML = '';
      if (!files.length) {
        status.innerText = 'No telemetry CSV files found (SD card missing or empty).';
        return;
      }
      files.forEach(f => {
        const opt = document.createElement('option');
        opt.value = f.name;
        opt.text = f.name;
        sel.appendChild(opt);
      });
      sel.selectedIndex = files.length - 1; // most recent
      loadGraph();
    } catch (e) {
      status.innerText = 'Failed to list log files: ' + e.message;
    }
  }

  async function loadGraph() {
    const sel = document.getElementById('fileSelect');
    const status = document.getElementById('status');
    if (!sel.value) return;
    document.getElementById('downloadLink').href = '/api/logs/download?file=' + encodeURIComponent(sel.value);
    status.innerText = 'Loading...';
    try {
      const res = await fetch('/api/logs/graph?file=' + encodeURIComponent(sel.value));
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const data = parseCSV(await res.text());
      status.innerText = data.labels.length + ' points shown (downsampled for display).';

      const vChart = darkChart('chartV', [
        { label: 'Pack V', data: data.packV, borderColor: '#4caf50', yAxisID: 'yV', pointRadius: 0 }
      ], {
        yV: { position: 'left', ticks: { color: '#4caf50' }, grid: { color: '#222' } }
      });
      vChart.data.labels = data.labels; vChart.update();

      const socChart = darkChart('chartSoc', [
        { label: 'SOC %', data: data.soc, borderColor: '#ff9800', yAxisID: 'ySoc', pointRadius: 0 }
      ], {
        ySoc: { position: 'left', min: 0, max: 100, ticks: { color: '#ff9800' }, grid: { color: '#222' } }
      });
      socChart.data.labels = data.labels; socChart.update();

      const iChart = darkChart('chartI', [
        { label: 'Pack Current (A)', data: data.packI, borderColor: '#2196F3', yAxisID: 'yI', pointRadius: 0 },
        { label: 'Requested Current (A)', data: data.reqI, borderColor: '#9c27b0', yAxisID: 'yI', pointRadius: 0 }
      ], {
        yI: { position: 'left', ticks: { color: '#ccc' }, grid: { color: '#222' } }
      });
      iChart.data.labels = data.labels; iChart.update();

      const cChart = darkChart('chartCell', [
        { label: 'Min Cell V', data: data.minC, borderColor: '#2196F3', yAxisID: 'yC', pointRadius: 0 },
        { label: 'Max Cell V', data: data.maxC, borderColor: '#f44336', yAxisID: 'yC', pointRadius: 0 }
      ], {
        yC: { position: 'left', ticks: { color: '#ccc' }, grid: { color: '#222' } }
      });
      cChart.data.labels = data.labels; cChart.update();
    } catch (e) {
      status.innerText = 'Failed to load graph: ' + e.message;
    }
  }

  loadList();
</script></body></html>)rawliteral";