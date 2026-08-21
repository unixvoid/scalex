const RELEASES_URL = 'https://api.github.com/repos/unixvoid/scalex/releases?per_page=30';
const REPOSITORY_URL = 'https://github.com/unixvoid/scalex';
const FIRMWARE_BASE_URL = 'https://unixvoid-builds.s3.amazonaws.com/scalex';
const installButton = document.querySelector('#espInstall');
const releaseSelect = document.querySelector('#releaseSelect');
const flashButton = document.querySelector('#flashButton');
const selectedVersion = document.querySelector('#selectedVersion');
const assetName = document.querySelector('#assetName');
const releaseLink = document.querySelector('#releaseLink');
const statusMessage = document.querySelector('#statusMessage');
const progressLabel = document.querySelector('#progressLabel');
const progressPercent = document.querySelector('#progressPercent');
const progressBar = document.querySelector('#progressBar');
const connectionState = document.querySelector('#connectionState span');
const connectionDot = document.querySelector('#connectionState i');

let releases = [];
let activeRelease = null;

function setStatus(message, label = 'Waiting for a device', percent = 0) {
  statusMessage.textContent = message;
  progressLabel.textContent = label;
  progressPercent.textContent = `${percent}%`;
  progressBar.style.width = `${percent}%`;
}

function getFirmwareAsset(release) {
  return release.assets.find((asset) => asset.name === `scalex-${release.tag_name}.bin`) || release.assets.find((asset) => asset.name.endsWith('.bin'));
}

function firmwareUrl(release, file) {
  return `${FIRMWARE_BASE_URL}/${release.tag_name}/${file}`;
}

function createManifest(release) {
  return URL.createObjectURL(new Blob([JSON.stringify({
    name: 'ScaleX',
    version: release.tag_name,
    builds: [{
      chipFamily: 'ESP32-C3',
      parts: [
        { path: firmwareUrl(release, 'bootloader.bin'), offset: 0x0 },
        { path: firmwareUrl(release, 'partition-table.bin'), offset: 0x8000 },
        { path: firmwareUrl(release, 'ota_data_initial.bin'), offset: 0x3d0000 },
        { path: firmwareUrl(release, 'app.bin'), offset: 0x10000 }
      ]
    }]
  })], { type: 'application/json' }));
}

function chooseRelease() {
  activeRelease = releases[Number(releaseSelect.value)];
  const asset = activeRelease && getFirmwareAsset(activeRelease);
  if (!asset) {
    flashButton.disabled = true;
    assetName.textContent = 'No .bin asset found';
    selectedVersion.textContent = 'Unavailable';
    setStatus('This release does not contain a compatible firmware image.');
    return;
  }

  assetName.textContent = asset.name;
  selectedVersion.textContent = activeRelease.tag_name;
  releaseLink.href = activeRelease.html_url || REPOSITORY_URL;
  installButton.manifest = createManifest(activeRelease);
  flashButton.disabled = false;
  setStatus('Connect your ScaleX device with a USB cable to begin.');
}

async function loadReleases() {
  try {
    const response = await fetch(RELEASES_URL, { headers: { Accept: 'application/vnd.github+json' } });
    if (!response.ok) throw new Error(`GitHub returned ${response.status}`);
    releases = (await response.json()).filter((release) => !release.draft && !release.prerelease && getFirmwareAsset(release));
    if (!releases.length) throw new Error('No compatible releases found');

    releaseSelect.replaceChildren(...releases.map((release) => {
      const option = new Option(`${release.tag_name}  /  ${new Date(release.published_at).toLocaleDateString()}`, releases.indexOf(release));
      return option;
    }));
    releaseSelect.disabled = false;
    chooseRelease();
  } catch (error) {
    selectedVersion.textContent = 'Unavailable';
    assetName.textContent = 'Could not load releases';
    statusMessage.textContent = 'GitHub releases could not be loaded. Check your connection and refresh the page.';
    releaseSelect.replaceChildren(new Option('Unable to load releases'));
  }
}

releaseSelect.addEventListener('change', chooseRelease);
flashButton.addEventListener('click', () => {
  if (!activeRelease) return;
  setStatus('Choose the USB serial port for your ScaleX device in the browser dialog.', 'Connecting', 8);
  connectionState.textContent = 'Select device';
  connectionDot.style.background = 'var(--orange)';
});

installButton.addEventListener('state-changed', (event) => {
  const state = event.detail?.state || event.detail;
  const labels = { CONNECTING: 'Connecting', INSTALLING: 'Flashing', FINISHED: 'Complete', ERROR: 'Error' };
  if (state === 'CONNECTING') setStatus('USB device selected. Opening the bootloader connection...', labels[state], 18);
  if (state === 'INSTALLING') setStatus(`Writing ${activeRelease.tag_name} to your ScaleX device...`, labels[state], 62);
  if (state === 'FINISHED') {
    setStatus('Firmware installed successfully. You can disconnect the device.', labels[state], 100);
    connectionState.textContent = 'Firmware current';
    connectionDot.style.background = 'var(--green)';
  }
  if (state === 'ERROR') {
    setStatus('The device could not be flashed. Keep it connected and try again.', labels[state], 0);
    connectionState.textContent = 'Ready to connect';
    connectionDot.style.background = 'var(--green)';
  }
});

loadReleases();
