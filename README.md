# ScaleX

ESP32 based smart scale with WiFi and BLE connectivity.

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
