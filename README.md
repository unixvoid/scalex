<div align="center">

# ScaleX

[![GitHub release](https://img.shields.io/github/v/release/unixvoid/scalex)](https://github.com/unixvoid/scalex/releases)
[![ESP32](https://img.shields.io/badge/ESP32-important?labelColor=333&colorA=e34c26&colorB=11b1e3&logoColor=white&logo=espressif)]()
[![Firmware flasher](https://img.shields.io/badge/%F0%9F%94%A5%20Flasher-docs%2F-green.svg)](https://unixvoid.github.io/scalex/)

**[Flash firmware directly from your browser → unixvoid.github.io/scalex](https://unixvoid.github.io/scalex/)**

</div>

## Overview

ScaleX is an ESP32-powered smart scale that exposes WiFi and BLE connectivity for easy setup, data streaming, and firmware updates. Need to flash a new build? Use the hosted [web flasher](https://unixvoid.github.io/scalex/) - it talks to your device right from the browser.


---

## Cutting a new version

1. Create a git tag for the release, for example:

   ```bash
   git tag v0.9.0
   git push origin v0.9.0
   ```

2. The GitHub Actions workflow will detect the tag and build the firmware with that version.
3. The build artifacts will be uploaded with the release version name.

For development or non-tag builds, the fallback version is `0.0.1`.

## Build behavior

- Pushes and pull requests to `main` run the build for CI/validation.
- Untagged builds use the fallback version `0.0.1`.
- Tagged builds like `v0.9.0` use the tag version and upload release artifacts.
- Only tag-triggered builds upload firmware artifacts.
