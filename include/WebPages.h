#pragma once
#include <Arduino.h>

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>BMS Bridge Pro</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; text-align: center; background: #121212; color: #e0e0e0; margin: 0; }
  .nav { background: #1e1e1e; padding: 10px; border-bottom: 2px solid #333; margin-bottom: 10px; }
  .nav a { color: #4caf50; text-decoration: none; margin: 0 15px; font-weight: bold; }
  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 10px; padding: 15px; }
  .card { background: #1e1e1e; padding: 15px; border-radius: 10px; border: 1px solid #333; }
  .value { font-size: 2em; font-weight: bold; color: #4caf50; }
  .err-active { color: #f44336 !important; }
  .btn { border: none; padding: 12px 20px; border-radius: 5px; cursor: pointer; font-weight: bold; margin: 10px; display: inline-block; text-decoration: none; color: white; }
  .btn-blue { background: #0277bd; } .btn-red { background: #d32f2f; }
  
  /* Cell Grid Styles */
  .cells-container { background: #1e1e1e; padding: 15px; border-radius: 10px; border: 1px solid #333; margin: 0 15px 15px 15px; }
  .cells-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(65px, 1fr)); gap: 8px; margin-top: 15px; }
  .cell-box { background: #2a2a2a; padding: 10px 5px; border-radius: 5px; font-size: 0.85em; border: 1px solid #444; color: #aaa; }
  .cell-box span { display: block; font-size: 1.25em; font-weight: bold; margin-top: 4px; color: #e0e0e0; }
  .cell-min { border-color: #2196F3; background: rgba(33, 150, 243, 0.1); }
  .cell-min span { color: #2196F3; }
  .cell-max { border-color: #f44336; background: rgba(244, 67, 54, 0.1); }
  .cell-max span { color: #f44336; }
  
  #console { width: 95%; max-width: 1000px; height: 300px; margin: 15px auto; background: #000; color: #00ff00; font-family: monospace; text-align: left; padding: 15px; overflow-y: scroll; border-radius: 8px; border: 1px solid #444; }
</style></head><body>
<div class="nav"><a href="/">DASHBOARD</a> | <a href="/config">CONFIGURATION</a></div>
<div class="grid">
  <div class="card"><div>Pack Voltage</div><div id="v" class="value">--</div></div>
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
<div id="console">Log Active...<br></div>
<script>
  var source = new EventSource('/events');
  source.addEventListener('data', function(e) {
    var obj = JSON.parse(e.data);
    document.getElementById('v').innerHTML = obj.v.toFixed(2) + " V";
    document.getElementById('cv').innerHTML = ((obj.maxC - obj.minC) * 1000).toFixed(0) + " mV";
    document.getElementById('i').innerHTML = obj.i.toFixed(1) + " A";
    document.getElementById('soc').innerHTML = obj.soc + "%";
    document.getElementById('smastat').innerHTML = (obj.err > 0 && obj.err < 60000) ? "ERROR "+obj.err : "OK";
    if(obj.err > 0 && obj.err < 60000) document.getElementById('smastat').className = "value err-active";
    else document.getElementById('smastat').className = "value";
    
    const mb = document.getElementById('mbtn');
    if(obj.force) { mb.innerHTML = "STOP FORCE CHARGE"; mb.style.background = "#ff9800"; }
    else { mb.innerHTML = "TRIGGER FORCE CHARGE"; mb.style.background = "#0277bd"; }

    // Render 16 cells
    if(obj.cells && obj.cells.length > 0) {
      var cg = document.getElementById('cGrid');
      var html = "";
      for(var i=0; i<obj.cells.length; i++) {
        var cls = "cell-box";
        if(obj.cells[i] === obj.minC) cls += " cell-min";
        else if(obj.cells[i] === obj.maxC) cls += " cell-max";
        html += "<div class='"+cls+"'>C"+(i+1)+"<span>"+obj.cells[i].toFixed(3)+"V</span></div>";
      }
      cg.innerHTML = html;
    }
  }, false);
  source.addEventListener('log', function(e) {
    const con = document.getElementById('console'); con.innerHTML += e.data + "<br>";
    if(con.childNodes.length > 100) con.removeChild(con.firstChild);
    con.scrollTop = con.scrollHeight;
  }, false);
</script></body></html>)rawliteral";

const char config_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><title>Settings</title><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; background: #121212; color: #eee; padding: 10px; }
  .container { max-width: 650px; margin: auto; background: #1e1e1e; padding: 25px; border-radius: 12px; border: 1px solid #333; }
  .row { display: flex; justify-content: space-between; align-items: center; margin-bottom: 12px; border-bottom: 1px solid #2a2a2a; padding-bottom: 8px; }
  .text-group { text-align: left; padding-right: 15px; }
  .desc { font-size: 0.8em; color: #888; display: block; margin-top: 2px; }
  h2 { color: #4caf50; border-bottom: 2px solid #4caf50; padding-bottom: 5px; margin-top: 25px; }
  .winter-h { color: #ff9800 !important; border-bottom: 2px solid #ff9800 !important; }
  input { font-size: 1.1em; padding: 5px; width: 110px; text-align: center; background: #000; color: #0f0; border: 1px solid #444; border-radius: 4px; }
  .save { background: #2e7d32; color: white; border: none; padding: 15px; width: 100%; border-radius: 5px; font-weight: bold; cursor: pointer; font-size: 1.1em; margin-top: 20px; }
</style></head><body>
<div class="container">
  <a href="/" style="color:#4caf50;text-decoration:none;">&larr; Back to Dashboard</a>
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
      <input type="number" name="ld" step="1" value="!!VAL_LIMP!!"></div>
    <div class="row"><div class="text-group"><strong>Min Discharge Vpc</strong><span class="desc">Absolute floor to prevent cell reversal.</span></div>
      <input type="number" name="cmdv" step="0.001" value="!!VAL_MDV!!"></div>

    <h2>System Tuning</h2>
    <div class="row"><div class="text-group"><strong>Voltage Window</strong><span class="desc">Number of moving average samples (1-20).</span></div>
      <input type="number" name="vs" step="1" value="!!VAL_VS!!"></div>

    <button type="submit" class="save">SAVE & APPLY ALL CHANGES</button>
  </form>
</div></body></html>)rawliteral";