#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
build_dir="${1:-build}"
version="${2:-1.0.0}"
case "$version" in *[!0-9.]*|'') echo 'Version must contain digits and dots only' >&2; exit 1;; esac
python3 scripts/check_storage.py --reserve-gib 2.5
artifacts="$build_dir/Aether_Matrix_artefacts/Release"
for bundle in 'AU/Aether Matrix.component' 'VST3/Aether Matrix.vst3' 'Standalone/Aether Matrix.app'; do
    test -d "$artifacts/$bundle" || { echo "Missing bundle: $artifacts/$bundle" >&2; exit 1; }
    /usr/bin/codesign --verify --deep --strict "$artifacts/$bundle"
done
stage="$(mktemp -d "$build_dir/aether-pkg.XXXXXX")"
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/root/Library/Audio/Plug-Ins/Components" \
    "$stage/root/Applications" \
    "$stage/root/Library/Audio/Plug-Ins/VST3" \
    "$stage/root/Library/Application Support/AetherAudioLabs/AetherMatrix" \
    "$stage/root/Library/Application Support/AetherAudioLabs/AetherMatrix/licenses" \
    "$stage/scripts" "$build_dir/installers"
ditto "$artifacts/AU/Aether Matrix.component" "$stage/root/Library/Audio/Plug-Ins/Components/Aether Matrix.component"
ditto "$artifacts/VST3/Aether Matrix.vst3" "$stage/root/Library/Audio/Plug-Ins/VST3/Aether Matrix.vst3"
ditto "$artifacts/Standalone/Aether Matrix.app" "$stage/root/Applications/Aether Matrix.app"
cp models/rave_decoder.onnx "$stage/root/Library/Application Support/AetherAudioLabs/AetherMatrix/"
cp LICENSE THIRD_PARTY_NOTICES.md "$stage/root/Library/Application Support/AetherAudioLabs/AetherMatrix/licenses/"
cp docs/licenses/* "$stage/root/Library/Application Support/AetherAudioLabs/AetherMatrix/licenses/"
cp installers/macos/postinstall "$stage/scripts/postinstall"
chmod 755 "$stage/scripts/postinstall"
# Disable bundle relocation so upgrades always target the documented system paths.
/usr/bin/pkgbuild --analyze --root "$stage/root" "$stage/components.plist"
/usr/bin/plutil -replace 0.BundleIsRelocatable -bool NO "$stage/components.plist"
/usr/bin/plutil -replace 1.BundleIsRelocatable -bool NO "$stage/components.plist"
/usr/bin/plutil -replace 2.BundleIsRelocatable -bool NO "$stage/components.plist"
/usr/bin/pkgbuild --root "$stage/root" --scripts "$stage/scripts" \
    --component-plist "$stage/components.plist" --identifier audio.aetherlabs.matrix \
    --version "$version" --install-location / "$stage/payload.pkg"
/usr/bin/productbuild --package "$stage/payload.pkg" "$build_dir/installers/Aether_Matrix_macOS.pkg"
/usr/sbin/pkgutil --check-signature "$build_dir/installers/Aether_Matrix_macOS.pkg" || true
