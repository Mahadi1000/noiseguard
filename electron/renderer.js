/**
 * NoiseGuard - Renderer Process (Vanilla JS)
 *
 * Handles UI interaction and communicates with main process via the
 * preload-exposed `window.noiseGuard` bridge.
 *
 * Features:
 *   - Device selection and toggle
 *   - Level meters (input RMS, output RMS, VAD)
 *   - Processing log confirming RNNoise activity
 *   - VAD gate threshold control
 */

/* ── DOM References ──────────────────────────────────────────────────────── */

const toggleBtn = document.getElementById('toggleBtn');
const toggleHint = document.getElementById('toggleHint');
const statusDot = document.getElementById('statusDot');
const inputSelect = document.getElementById('inputSelect');
const outputSelect = document.getElementById('outputSelect');
const levelSlider = document.getElementById('levelSlider');
const levelValue = document.getElementById('levelValue');
const vadThreshSlider = document.getElementById('vadThreshSlider');
const vadThreshValue = document.getElementById('vadThreshValue');
const statusText = document.getElementById('statusText');
const latencyText = document.getElementById('latencyText');
const framesText = document.getElementById('framesText');
const gateText = document.getElementById('gateText');
const errorBar = document.getElementById('errorBar');

/* Meters */
const inputMeter = document.getElementById('inputMeter');
const outputMeter = document.getElementById('outputMeter');
const inputDb = document.getElementById('inputDb');
const outputDb = document.getElementById('outputDb');
const vadBar = document.getElementById('vadBar');
const vadValue = document.getElementById('vadValue');
const meterSection = document.getElementById('meterSection');

/* Log */
const logContainer = document.getElementById('logContainer');
const logSection = document.getElementById('logSection');

/* ── State ───────────────────────────────────────────────────────────────── */

let isRunning = false;
let metricsInterval = null;
let lastFrameCount = 0;
let logLines = 0;
const MAX_LOG_LINES = 50;

/* ── Initialize ──────────────────────────────────────────────────────────── */

async function init() {
  await loadDevices();
  await syncStatus();

  /* Poll status every 2 seconds for external state changes. */
  setInterval(syncStatus, 2000);
}

/** Load available audio devices into the dropdown selects. */
async function loadDevices() {
  try {
    const devices = await window.noiseGuard.getDevices();

    if (devices.error) {
      showError(devices.error);
      return;
    }

    populateSelect(inputSelect, devices.inputs, 'input');
    populateSelect(outputSelect, devices.outputs, 'output');

    hideError();
  } catch (err) {
    showError('Failed to load audio devices: ' + err.message);
  }
}

/** Populate a <select> with device options. */
function populateSelect(select, devices, type) {
  select.innerHTML = '<option value="-1">System Default</option>';

  if (type === 'output') {
    select.innerHTML += '<option value="-2">No Output (Mute)</option>';
  }

  for (const d of devices) {
    const opt = document.createElement('option');
    opt.value = d.index;
    opt.textContent = d.name;

    if (d.name.toLowerCase().includes('cable')) {
      opt.textContent += ' [VB-Cable]';
    }

    select.appendChild(opt);
  }
}

/** Sync UI with engine status. */
async function syncStatus() {
  try {
    const status = await window.noiseGuard.getStatus();
    updateUI(status.running, status.level);
  } catch (err) {
    /* Silently ignore polling errors. */
  }
}

/* ── Toggle Noise Cancellation ───────────────────────────────────────────── */

toggleBtn.addEventListener('click', async () => {
  toggleBtn.disabled = true;

  try {
    if (isRunning) {
      const result = await window.noiseGuard.stop();
      if (result.success) {
        updateUI(false);
        addLog('Engine stopped', 'warn');
      } else {
        showError(result.error || 'Failed to stop');
      }
    } else {
      const inputIdx = parseInt(inputSelect.value, 10);
      const outputIdx = parseInt(outputSelect.value, 10);

      statusText.textContent = 'Starting...';
      addLog('Starting engine...', 'ok');
      const result = await window.noiseGuard.start(inputIdx, outputIdx);

      if (result.success) {
        updateUI(true);
        hideError();
        addLog('Engine started. RNNoise processing frames.', 'ok');
      } else {
        showError(result.error || 'Failed to start');
        statusText.textContent = 'Error';
        addLog('Start failed: ' + (result.error || 'unknown'), 'warn');
      }
    }
  } catch (err) {
    showError(err.message);
  } finally {
    toggleBtn.disabled = false;
  }
});

/* ── Suppression Level Slider ────────────────────────────────────────────── */

levelSlider.addEventListener('input', () => {
  const pct = parseInt(levelSlider.value, 10);
  levelValue.textContent = pct + '%';
});

levelSlider.addEventListener('change', async () => {
  const level = parseInt(levelSlider.value, 10) / 100.0;
  try {
    await window.noiseGuard.setLevel(level);
  } catch (err) { /* Non-critical */ }
});

/* ── VAD Gate Threshold Slider ───────────────────────────────────────────── */

vadThreshSlider.addEventListener('input', () => {
  const pct = parseInt(vadThreshSlider.value, 10);
  vadThreshValue.textContent = pct + '%';
});

vadThreshSlider.addEventListener('change', async () => {
  const threshold = parseInt(vadThreshSlider.value, 10) / 100.0;
  try {
    await window.noiseGuard.setVadThreshold(threshold);
    addLog('VAD threshold set to ' + (threshold * 100).toFixed(0) + '%', 'ok');
  } catch (err) { /* Non-critical */ }
});

/* ── Device selection change while running -> restart ────────────────────── */

inputSelect.addEventListener('change', restartIfRunning);
outputSelect.addEventListener('change', restartIfRunning);

async function restartIfRunning() {
  if (!isRunning) return;

  await window.noiseGuard.stop();
  const inputIdx = parseInt(inputSelect.value, 10);
  const outputIdx = parseInt(outputSelect.value, 10);

  statusText.textContent = 'Restarting...';
  addLog('Restarting with new devices...', 'ok');
  const result = await window.noiseGuard.start(inputIdx, outputIdx);

  if (result.success) {
    updateUI(true);
    hideError();
    addLog('Restarted successfully.', 'ok');
  } else {
    showError(result.error || 'Restart failed');
    updateUI(false);
    addLog('Restart failed: ' + (result.error || 'unknown'), 'warn');
  }
}

/* ── Metrics Polling ─────────────────────────────────────────────────────── */

function startMetricsPolling() {
  if (metricsInterval) return;

  lastFrameCount = 0;
  metricsInterval = setInterval(async () => {
    try {
      const m = await window.noiseGuard.getMetrics();

      /* Level meters (RMS -> percentage, with log scaling for dB feel) */
      const inPct = rmsToPercent(m.inputRms);
      const outPct = rmsToPercent(m.outputRms);

      inputMeter.style.width = inPct + '%';
      outputMeter.style.width = outPct + '%';
      inputDb.textContent = rmsToDb(m.inputRms);
      outputDb.textContent = rmsToDb(m.outputRms);

      /* VAD bar */
      const vadPct = Math.round(m.vadProbability * 100);
      vadBar.style.width = vadPct + '%';
      vadValue.textContent = vadPct + '%';

      /* Status fields */
      framesText.textContent = formatFrameCount(m.framesProcessed);
      gateText.textContent = (m.gateGain * 100).toFixed(0) + '%';

      /* Log periodic confirmation that RNNoise is processing */
      if (m.framesProcessed > 0 && m.framesProcessed !== lastFrameCount) {
        const delta = m.framesProcessed - lastFrameCount;
        if (lastFrameCount > 0 && delta > 0 && m.framesProcessed % 500 < delta) {
          addLog(
            'RNNoise: ' + m.framesProcessed + ' frames | ' +
            'VAD=' + (m.vadProbability * 100).toFixed(0) + '% | ' +
            'Gate=' + (m.gateGain * 100).toFixed(0) + '%',
            'ok'
          );
        }
        lastFrameCount = m.framesProcessed;
      }
    } catch (err) { /* Ignore polling errors */ }
  }, 100);
}

function stopMetricsPolling() {
  if (metricsInterval) {
    clearInterval(metricsInterval);
    metricsInterval = null;
  }
  /* Reset meters */
  inputMeter.style.width = '0%';
  outputMeter.style.width = '0%';
  vadBar.style.width = '0%';
  inputDb.textContent = '-\u221E';
  outputDb.textContent = '-\u221E';
  vadValue.textContent = '0%';
  framesText.textContent = '0';
  gateText.textContent = '--';
}

/** Convert RMS [0..1] to a display percentage (log-scaled). */
function rmsToPercent(rms) {
  if (rms <= 0.0001) return 0;
  /* Map -60dB..0dB to 0..100% */
  const db = 20 * Math.log10(Math.max(rms, 0.000001));
  return Math.max(0, Math.min(100, ((db + 60) / 60) * 100));
}

/** Convert RMS to dB string. */
function rmsToDb(rms) {
  if (rms <= 0.0001) return '-\u221E';
  const db = 20 * Math.log10(rms);
  return db.toFixed(0) + 'dB';
}

/** Format large frame counts with K/M suffix. */
function formatFrameCount(n) {
  if (n >= 1000000) return (n / 1000000).toFixed(1) + 'M';
  if (n >= 1000) return (n / 1000).toFixed(1) + 'K';
  return String(n);
}

/* ── Processing Log ──────────────────────────────────────────────────────── */

function addLog(message, type) {
  const entry = document.createElement('div');
  entry.className = 'log-entry';

  const now = new Date();
  const timeStr = now.toLocaleTimeString('en-US', { hour12: false });

  entry.innerHTML =
    '<span class="log-time">[' + timeStr + ']</span> ' +
    '<span class="log-' + (type || 'ok') + '">' + message + '</span>';

  logContainer.appendChild(entry);
  logLines++;

  /* Trim old entries */
  while (logLines > MAX_LOG_LINES) {
    logContainer.removeChild(logContainer.firstChild);
    logLines--;
  }

  /* Auto-scroll to bottom */
  logContainer.scrollTop = logContainer.scrollHeight;
}

/* ── UI Update Helpers ───────────────────────────────────────────────────── */

function updateUI(running, level) {
  isRunning = running;

  toggleBtn.classList.toggle('on', running);
  toggleBtn.classList.toggle('off', !running);
  toggleBtn.querySelector('.toggle-label').textContent = running ? 'ON' : 'OFF';

  toggleHint.textContent = running
    ? 'Noise cancellation active'
    : 'Click to enable noise cancellation';

  statusDot.classList.toggle('active', running);

  statusText.textContent = running ? 'Active' : 'Idle';

  if (!running) {
    latencyText.textContent = '-- ms';
  } else if (parseInt(outputSelect.value, 10) === -2) {
    latencyText.textContent = 'Muted';
  } else {
    latencyText.textContent = '~12 ms';
  }

  inputSelect.disabled = false;
  outputSelect.disabled = false;

  if (level !== undefined) {
    const pct = Math.round(level * 100);
    levelSlider.value = pct;
    levelValue.textContent = pct + '%';
  }

  /* Start/stop metrics polling */
  if (running) {
    startMetricsPolling();
  } else {
    stopMetricsPolling();
  }
}

function showError(msg) {
  errorBar.textContent = msg;
  errorBar.classList.remove('hidden');
}

function hideError() {
  errorBar.classList.add('hidden');
  errorBar.textContent = '';
}

/* ── Boot ────────────────────────────────────────────────────────────────── */

init();
