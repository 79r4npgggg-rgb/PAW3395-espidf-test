'use strict';

const linkChip = document.getElementById('link');
const infoView = document.getElementById('info');

async function pollInfo() {
  try {
    const response = await fetch('/api/info', { cache: 'no-store' });
    if (!response.ok) throw new Error('HTTP ' + response.status);
    const info = await response.json();
    linkChip.textContent = 'ESP32: 応答あり';
    linkChip.className = 'chip ok';
    infoView.textContent = JSON.stringify(info, null, 2);
  } catch (error) {
    linkChip.textContent = 'ESP32: 応答なし (' + error.message + ')';
    linkChip.className = 'chip bad';
  }
}

pollInfo();
setInterval(pollInfo, 2000);
