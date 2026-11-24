# Publishing firmware releases

This repository is a board-specific fork of `78/xiaozhi-esp32`. It does **not**
use the upstream release workflow (`.github/workflows/release.yml`) or
`scripts/publish_firmware.py`; those files are not present here and are not
required for this board. Releases are produced manually with the single
landscape merged image used by the board.

## Version

The release version comes from `PROJECT_VER` in the root `CMakeLists.txt`.
Update it before building a new release. Do not reuse a tag or version that has
already been published.

## Build the release image

The board is published in landscape orientation (400x300), selected at compile
time by `RLCD_PORTRAIT 0` in
`main/boards/waveshare/esp32-s3-rlcd-4.2/config.h`.

```sh
# config.h: #define RLCD_PORTRAIT 0
python scripts/build.py waveshare/esp32-s3-rlcd-4.2 --language zh-CN
cp build/merged-binary.bin dist/merged-binary-landscape.bin
```

`scripts/build.py` runs `idf.py merge-bin` and writes the complete image to
`build/merged-binary.bin`; it contains the full flash layout starting at
offset 0.

Portrait is a local development option only. To try it, set
`RLCD_PORTRAIT 1` and rebuild locally; portrait images are not published as
release assets.

## Verify before publishing

- Build log ends with `Project build complete`.
- `build/project_description.json` has the intended `project_version`.
- Flash the merged image to the board and smoke-test boot, Wi-Fi, display,
  audio, and the changed feature.
- Record the SHA-256 of the image.

## Tag, push, and publish

```sh
git add dist/merged-binary-landscape.bin
git commit -m "chore(release): rebuild merged image for <version>"
git tag v<version>
git push newrepo main
git push newrepo v<version>

gh release create v<version> \
  -R kid941005/xiaozhi-esp32-s3-rlcd-4.2 \
  --verify-tag \
  --title "v<version> — <summary>" \
  --notes-file release-notes.md \
  dist/merged-binary-landscape.bin
```

Only `merged-binary-landscape.bin` is published. After publishing, verify the
uploaded asset SHA-256 matches the local `dist/` file.

Successful compilation is not hardware validation. Test the relevant hardware
before recommending a release for use.
