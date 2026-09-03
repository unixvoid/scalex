const RELEASES_URL = 'https://api.github.com/repos/unixvoid/scalex/releases?per_page=30';
const REPOSITORY_URL = 'https://github.com/unixvoid/scalex';
const FIRMWARE_BASE_URL = 'https://unixvoid-builds.s3.amazonaws.com/scalex';
const installButton = document.querySelector('#espInstall');
const releaseSelect = document.querySelector('#releaseSelect');
const flashButton = document.querySelector('#flashButton');
const releaseLink = document.querySelector('#releaseLink');

let releases = [];
let activeRelease = null;

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
    new_install_prompt_erase: true,
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
    return;
  }

  releaseLink.href = activeRelease.html_url || REPOSITORY_URL;
  installButton.manifest = createManifest(activeRelease);
  flashButton.disabled = false;
}

async function loadReleases() {
  try {
    const response = await fetch(RELEASES_URL, { headers: { Accept: 'application/vnd.github+json' } });
    if (!response.ok) throw new Error(`GitHub returned ${response.status}`);
    releases = (await response.json())
      .filter((release) => !release.draft && !release.prerelease && getFirmwareAsset(release))
      .sort((a, b) => {
        const versionA = a.tag_name.match(/^v?(\d+)\.(\d+)\.(\d+)(?:[-+].*)?$/);
        const versionB = b.tag_name.match(/^v?(\d+)\.(\d+)\.(\d+)(?:[-+].*)?$/);

        if (versionA && versionB) {
          for (let i = 1; i <= 3; i += 1) {
            const diff = Number(versionB[i]) - Number(versionA[i]);
            if (diff) return diff;
          }
        }

        return new Date(b.published_at) - new Date(a.published_at);
      });
    if (!releases.length) throw new Error('No compatible releases found');

    releaseSelect.replaceChildren(...releases.map((release, index) => {
      const option = new Option(`${release.tag_name}  /  ${new Date(release.published_at).toLocaleDateString()}`, index);
      return option;
    }));
    releaseSelect.disabled = false;
    chooseRelease();
  } catch (error) {
    releaseSelect.replaceChildren(new Option('Unable to load releases'));
  }
}

releaseSelect.addEventListener('change', chooseRelease);

loadReleases();
