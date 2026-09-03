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
   git tag 1.0.2
   git push origin 1.0.2
   ```

   Both `1.0.2` and `v1.0.2` style tags work — any tag that looks like a version
   triggers the build.

2. The GitHub Actions workflow detects the tag and builds the firmware. The version is resolved automatically by ESP-IDF from the git tag (`git describe --tags`), so the tag is the single source of truth. That version is embedded in the firmware and shown at the top of the Settings page in the device web UI.
3. The build artifacts are uploaded under the release version name, and a GitHub Release is published.

> **Note:** the web flasher only lists releases that have firmware assets. If a release
> doesn't show up in the flasher dropdown, its tag build didn't produce artifacts
> (for example, the tag was created from the GitHub UI before the workflow existed,
> or it isn't a `v*`/`<digit>*` tag).

## Build behavior

- Pushes and pull requests to `main` run the build for CI/validation.
- Git tags are the source of truth for the version. Tagged builds like `1.0.2` embed that exact version and upload release artifacts.
- Untagged/dev builds embed whatever `git describe` resolves to (e.g. `1.0.2-3-ga1b2c3d`).
- Only tag-triggered builds upload firmware artifacts.
