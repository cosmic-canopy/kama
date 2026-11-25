#!/bin/sh
# check-ios-app.sh — an iOS app (KR-281 decisions 7 to 10): the manifest's `app.ios` and `app.orientation` rules
# everywhere, and, on a Mac with Xcode, the `.app` `kama build` makes for the simulator, read back by Apple's own tools.
#
# A. Needs nothing but kama. `app.orientation` is "any", "portrait" or "landscape"; `app.ios` is a closed object: a Team
#    ID is ten capital letters and digits, `devices` names "iphone" and "ipad" each once, `profiles` names `development`
#    and `distribution`, and `requires` is a library's, which states nothing else of `app.ios`. Before anything compiles,
#    a device build without a team is refused, and so are an `-o` that is not an `.app` (simulator) or `.ipa` (device), a
#    named file that does not exist, an icon that is not a `.icon` or an opaque 1024x1024 PNG (read from its header), a
#    privacy manifest under another name than Apple reads, an App Store Connect key named by only some of its three
#    variables, and `kama run --release` for a device, which only the App Store installs.
# B. On a Mac whose Xcode has the simulator's SDK: the `.app` for aarch64-ios17.0-simulator. Its Info.plist holds what
#    kama writes from kama.json (7a–7c) and Xcode's twelve; the project is the same bytes on a rebuild (8a); an
#    `ios.plist` adds its keys and is refused where it states one kama writes (7d); both icon forms reach the bundle, and
#    a change from one to the other leaves nothing of the first (7e); a --release build that reads a file's metadata is
#    refused without a privacy manifest giving a reason, and builds with one, which the bundle carries (7f); a package's
#    `ios.requires` is refused until the app states each key, in its plist or its entitlements (7g); `assets` land at
#    the bundle's root, beside the executable. Running the app needs a simulator, which the iOS leg makes (./dev test ios).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-ios-app: FAIL — $1" >&2; [ -f "$tmp/log" ] && sed -n '1,30p' "$tmp/log" >&2; exit 1; }

# app <dir> <kind> <app json> — a one-file project whose `main` returns 7.
app() {
    mkdir -p "$1/src"
    printf '{\n  "name": "hello",\n  "version": "1.2.3",\n  "kind": "%s",\n  "entry": "src/main.kama",\n  "source": "src",\n%b  "modules": { ".": { "visibility": "internal" } }\n}\n' \
        "$2" "$3" > "$1/kama.json"
    printf 'fn uint8 main()\n{\n    return 7;\n}\n' > "$1/src/main.kama"
}
# refused <name> <substring> <kind> <app json> — `kama check` refuses the manifest, saying <substring>.
refused() {
    app "$tmp/$1" "$3" "$4"
    if "$KAMA" check "$tmp/$1/kama.json" > "$tmp/log" 2>&1; then fail "$1: \`kama check\` accepted it"; fi
    grep -qF -- "$2" "$tmp/log" || fail "$1: the refusal does not say \"$2\""
}
# before <name> <substring> <app json> <kama args…> — the command is refused before anything compiles, saying <substring>.
before() {
    n=$1; want=$2; json=$3; shift 3
    app "$tmp/$n" app "$json"
    if "$KAMA" "$@" > "$tmp/log" 2>&1; then fail "$n: \`kama $*\` was not refused"; fi
    grep -qF -- "$want" "$tmp/log" || fail "$n: the refusal does not say \"$want\""
}
# pnghead <file> <size byte: \004 for 1024, \002 for 512> <colour type> — a PNG's signature and header, all the check reads.
pnghead() { printf "\\211PNG\\r\\n\\032\\n\\000\\000\\000\\015IHDR\\000\\000$2\\000\\000\\000$2\\000\\010$3\\000\\000\\000\\000\\000\\000\\000" > "$1"; }
ID='"id": "com.example.kamahello"'

# ---- A ---------------------------------------------------------------------------------------------------------------
refused orient    '`app.orientation` is "any", "portrait" or "landscape"' app "  \"app\": { $ID, \"orientation\": \"sideways\" },\n"
refused iosunk    'unknown key `minimum` in `app.ios`'      app "  \"app\": { $ID, \"ios\": { \"minimum\": \"17.0\" } },\n"
refused team      'is not a Team ID'                        app "  \"app\": { $ID, \"ios\": { \"team\": \"abcde12345\" } },\n"
refused devempty  'cannot be empty'                         app "  \"app\": { $ID, \"ios\": { \"devices\": [] } },\n"
refused devwatch  'is not "iphone" or "ipad"'               app "  \"app\": { $ID, \"ios\": { \"devices\": [\"watch\"] } },\n"
refused devtwice  'names "ipad" twice'                      app "  \"app\": { $ID, \"ios\": { \"devices\": [\"ipad\", \"ipad\"] } },\n"
refused profkey   'unknown key `adhoc` in `app.ios.profiles`' app "  \"app\": { $ID, \"ios\": { \"profiles\": { \"adhoc\": \"x\" } } },\n"
refused profempty 'cannot be empty or hold'                 app "  \"app\": { $ID, \"ios\": { \"profiles\": { \"development\": \"\" } } },\n"
refused appreq    "\`app.ios.requires\` is a library's"     app "  \"app\": { $ID, \"ios\": { \"requires\": [\"NSCameraUsageDescription\"] } },\n"
refused reqname   "is not a key's name"                     library '  "app": { "ios": { "requires": ["Camera usage"] } },\n'
refused libteam   'states only what an API it binds needs'  library '  "app": { "ios": { "team": "ABCDE12345" } },\n'
app "$tmp/libreq" library '  "app": { "ios": { "requires": ["NSCameraUsageDescription", "com.apple.developer.game-center"] } },\n'
"$KAMA" check "$tmp/libreq/kama.json" > "$tmp/log" 2>&1 || fail "a library's \`ios.requires\` was refused"

dev=aarch64-ios17.0-none; sim=aarch64-ios17.0-simulator
before noteam   'is signed for a team'     "  \"app\": { $ID },\n" build "$tmp/noteam/kama.json" --target $dev
before outext   'ends in `.app`'           "  \"app\": { $ID },\n" build "$tmp/outext/kama.json" --target $sim -o "$tmp/outext/hello.zip"
before outipa   'ends in `.ipa`'           "  \"app\": { $ID, \"ios\": { \"team\": \"ABCDE12345\" } },\n" \
    build "$tmp/outipa/kama.json" --target $dev -o "$tmp/outipa/hello.app"
before noplist  'does not exist'           "  \"app\": { $ID, \"ios\": { \"plist\": \"ios/Info.plist\" } },\n" build "$tmp/noplist/kama.json" --target $sim
app "$tmp/privname" app "  \"app\": { $ID, \"ios\": { \"privacy\": \"privacy.plist\" } },\n"
: > "$tmp/privname/privacy.plist"
if "$KAMA" build "$tmp/privname/kama.json" --target $sim > "$tmp/log" 2>&1; then fail "a privacy manifest under another name was accepted"; fi
grep -qF 'only under the name' "$tmp/log" || fail "a privacy manifest under another name was refused for another reason"
app "$tmp/icons" app "  \"app\": { $ID, \"ios\": { \"icon\": \"icon.png\" } },\n"
pnghead "$tmp/icons/icon.png" '\004' '\006'
if "$KAMA" build "$tmp/icons/kama.json" --target $sim > "$tmp/log" 2>&1; then fail "an icon with alpha was accepted"; fi
grep -qF 'has an alpha channel' "$tmp/log" || fail "an icon with alpha was refused for another reason"
pnghead "$tmp/icons/icon.png" '\002' '\002'
if "$KAMA" build "$tmp/icons/kama.json" --target $sim > "$tmp/log" 2>&1; then fail "a 512-pixel icon was accepted"; fi
grep -qF 'is 512x512 pixels' "$tmp/log" || fail "a 512-pixel icon was refused for another reason"
printf 'GIF89a' > "$tmp/icons/icon.png"
if "$KAMA" build "$tmp/icons/kama.json" --target $sim > "$tmp/log" 2>&1; then fail "a GIF named .png was accepted as an icon"; fi
grep -qF 'is not a PNG' "$tmp/log" || fail "a GIF named .png was refused for another reason"
app "$tmp/ascpart" app "  \"app\": { $ID, \"ios\": { \"team\": \"ABCDE12345\" } },\n"
if KAMA_ASC_KEY_PATH=/nowhere/AuthKey.p8 "$KAMA" build "$tmp/ascpart/kama.json" --target $dev > "$tmp/log" 2>&1; then
    fail "an App Store Connect key named by one variable of three was accepted"
fi
grep -qF '$KAMA_ASC_KEY_ID, $KAMA_ASC_ISSUER_ID is not set' "$tmp/log" || fail "a partial App Store Connect key was refused for another reason"
if "$KAMA" run "$tmp/ascpart/kama.json" --target $dev --release > "$tmp/log" 2>&1; then fail "\`kama run --release\` for a device was taken"; fi
grep -qF 'signed for the App Store' "$tmp/log" || fail "\`kama run --release\` for a device was refused for another reason"

# ---- B ---------------------------------------------------------------------------------------------------------------
if [ "$(uname -s)" != Darwin ] || ! xcrun --sdk iphonesimulator --show-sdk-path > /dev/null 2>&1 || ! command -v plutil > /dev/null 2>&1; then
    echo "check-ios-app: PASS (A: the manifest's rules and what a build refuses before it compiles; B skipped — no Xcode with the" \
         "simulator's SDK)"
    exit 0
fi
# png <file> <size> [alpha] — an image of one colour, by Python's zlib, which Xcode's tools bring.
png() {
    /usr/bin/python3 - "$@" <<'EOF'
import struct, sys, zlib
path, size = sys.argv[1], int(sys.argv[2]); alpha = len(sys.argv) > 3
px = b"\x30\x90\xc0" + (b"\xff" if alpha else b"")
raw = b"".join(b"\x00" + px * size for _ in range(size))
ch = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + ch(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6 if alpha else 2, 0, 0, 0))
                       + ch(b"IDAT", zlib.compress(raw, 9)) + ch(b"IEND", b""))
EOF
}
b() { "$KAMA" build "$1/kama.json" --target $sim ${2:-} > "$tmp/log" 2>&1; }
key() { plutil -extract "$2" raw -o - "$1" 2>/dev/null; }

app "$tmp/b" app "  \"app\": { $ID, \"label\": \"Hello & Co\", \"orientation\": \"landscape\", \"ios\": { \"devices\": [\"iphone\"] } },\n"
b "$tmp/b" || fail "the app did not build for $sim"
out="$tmp/b/out/$sim/debug"
pl="$out/hello.app/Info.plist"
[ -f "$out/hello.app/hello" ] || fail "the .app has no executable"
[ -f "$out/xcode/hello.xcodeproj/project.pbxproj" ] && [ -f "$out/xcode/libhello.a" ] && [ -f "$out/xcode/kama_app.c" ] \
    || fail "xcode/ beside the app lacks the project, the library or kama_app.c"
for kv in CFBundleIdentifier=com.example.kamahello "CFBundleDisplayName=Hello & Co" CFBundleExecutable=hello CFBundleName=hello \
          CFBundleShortVersionString=1.2.3 CFBundleVersion=1.2.3 CFBundlePackageType=APPL CFBundleDevelopmentRegion=en \
          CADisableMinimumFrameDurationOnPhone=true LSRequiresIPhoneOS=true MinimumOSVersion=17.0 \
          UIApplicationSceneManifest.UIApplicationSupportsMultipleScenes=false UIDeviceFamily.0=1 \
          UISupportedInterfaceOrientations.0=UIInterfaceOrientationLandscapeLeft \
          UISupportedInterfaceOrientations.1=UIInterfaceOrientationLandscapeRight DTPlatformName=iphonesimulator; do
    [ "$(key "$pl" "${kv%%=*}")" = "${kv#*=}" ] || { plutil -p "$pl" > "$tmp/log"; fail "the plist's ${kv%%=*} is not \"${kv#*=}\""; }
done
[ -z "$(key "$pl" UIDeviceFamily.1)" ] || fail "an iPhone app's UIDeviceFamily names an iPad too"
[ -z "$(key "$pl" 'UISupportedInterfaceOrientations~ipad')" ] || fail "an iPhone app has an iPad's orientations"
codesign -dv "$out/hello.app" 2>&1 | grep -qF 'Signature=adhoc' || fail "the simulator's app is not signed ad hoc"
sum1=$(cksum < "$out/xcode/hello.xcodeproj/project.pbxproj")
b "$tmp/b" || fail "the app did not build a second time"
[ "$(cksum < "$out/xcode/hello.xcodeproj/project.pbxproj")" = "$sum1" ] || fail "a rebuild wrote the project with other bytes"

# 7d: the app's own keys, added; one kama writes, refused.
mkdir -p "$tmp/b/ios"
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>NSCameraUsageDescription</key><string>Scan a level</string><key>ITSAppUsesNonExemptEncryption</key><false/><key>CFBundleDevelopmentRegion</key><string>fr</string></dict></plist>\n' > "$tmp/b/ios/Info.plist"
app "$tmp/b" app "  \"app\": { $ID, \"ios\": { \"plist\": \"ios/Info.plist\" } },\n"
b "$tmp/b" || fail "an app with its own plist keys did not build"
[ "$(key "$pl" NSCameraUsageDescription)" = "Scan a level" ] && [ "$(key "$pl" ITSAppUsesNonExemptEncryption)" = false ] \
    || fail "the app's own plist keys are not in the bundle's plist"
[ "$(key "$pl" CFBundleDevelopmentRegion)" = fr ] || fail "the app's own development region did not replace kama's default"
[ "$(key "$pl" 'UISupportedInterfaceOrientations~ipad.3')" = UIInterfaceOrientationLandscapeRight ] \
    || fail "an app held any way, on iPhone and iPad, does not give the iPad all four orientations"
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleVersion</key><string>9</string></dict></plist>\n' > "$tmp/b/ios/Info.plist"
if b "$tmp/b"; then fail "an app's plist stating CFBundleVersion was accepted"; fi
grep -qF "states \`CFBundleVersion\`, which kama writes from kama.json's \`version\`" "$tmp/log" || fail "CFBundleVersion was refused for another reason"

# 7e: both icon forms, and a change from one to the other.
app "$tmp/b" app "  \"app\": { $ID, \"ios\": { \"icon\": \"ios/icon.png\" } },\n"
png "$tmp/b/ios/icon.png" 1024
b "$tmp/b" || fail "an app with a PNG icon did not build"
[ -f "$out/hello.app/Assets.car" ] && [ "$(key "$pl" CFBundleIcons.CFBundlePrimaryIcon.CFBundleIconName)" = AppIcon ] \
    || fail "the PNG icon did not become Assets.car and CFBundleIcons"
mkdir -p "$tmp/b/ios/Game.icon/Assets"
png "$tmp/b/ios/Game.icon/Assets/layer.png" 1024
printf '{\n  "fill" : "automatic",\n  "groups" : [ { "layers" : [ { "image-name" : "layer.png", "name" : "layer" } ] } ],\n  "supported-platforms" : { "squares" : "shared" }\n}\n' > "$tmp/b/ios/Game.icon/icon.json"
app "$tmp/b" app "  \"app\": { $ID, \"ios\": { \"icon\": \"ios/Game.icon\" } },\n"
b "$tmp/b" || fail "an app with an Icon Composer icon did not build"
[ "$(key "$pl" CFBundleIcons.CFBundlePrimaryIcon.CFBundleIconName)" = Game ] || fail "the .icon is not the bundle's icon"
if ls "$out/hello.app" | grep -q '^AppIcon'; then fail "a change from the PNG icon to the .icon left the PNG's files in the bundle"; fi

# 7f: a --release build that reads a file's metadata, and the privacy manifest that gives its reason.
mkdir -p "$tmp/m/src" "$tmp/m/ios"
app "$tmp/m" app "  \"app\": { $ID },\n"
printf 'import { std::fs::stat };\nfn uint8 main()\n{\n    if (stat(path: "/").isOk()) { return 7; }\n    return 1;\n}\n' > "$tmp/m/src/main.kama"
if b "$tmp/m" --release; then fail "a --release app that reads a file's metadata built with no privacy manifest"; fi
grep -qF 'NSPrivacyAccessedAPICategoryFileTimestamp: `' "$tmp/log" || fail "the missing privacy manifest was refused for another reason"
b "$tmp/m" || fail "a debug build checked the privacy manifest"
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>NSPrivacyAccessedAPITypes</key><array><dict><key>NSPrivacyAccessedAPIType</key><string>NSPrivacyAccessedAPICategoryFileTimestamp</string><key>NSPrivacyAccessedAPITypeReasons</key><array><string>C617.1</string></array></dict></array></dict></plist>\n' > "$tmp/m/ios/PrivacyInfo.xcprivacy"
app "$tmp/m" app "  \"app\": { $ID, \"ios\": { \"privacy\": \"ios/PrivacyInfo.xcprivacy\" } },\n"
printf 'import { std::fs::stat };\nfn uint8 main()\n{\n    if (stat(path: "/").isOk()) { return 7; }\n    return 1;\n}\n' > "$tmp/m/src/main.kama"
b "$tmp/m" --release || fail "a --release app whose privacy manifest gives the reason did not build"
[ -f "$tmp/m/out/$sim/release/hello.app/PrivacyInfo.xcprivacy" ] || fail "the privacy manifest is not at the bundle's root"

# 7g: a package's requirement, refused until the app states each key.
mkdir -p "$tmp/r/vendor/cam/src" "$tmp/r/src" "$tmp/r/ios"
printf '{\n  "name": "cam",\n  "version": "1.0.0",\n  "kind": "library",\n  "source": "src",\n  "app": { "ios": { "requires": ["NSCameraUsageDescription", "com.apple.developer.game-center"] } },\n  "modules": { ".": { "visibility": "public" } }\n}\n' > "$tmp/r/vendor/cam/kama.json"
printf 'export { ping };\nfn int32 ping() { return 7; }\n' > "$tmp/r/vendor/cam/src/cam.kama"
req() {
    printf '{\n  "name": "hello",\n  "version": "1.2.3",\n  "kind": "app",\n  "entry": "src/main.kama",\n  "source": "src",\n  "dependencies": { "cam": { "path": "./vendor/cam" } },\n  "app": { %s, "ios": { %s } },\n  "modules": { ".": { "visibility": "internal" } }\n}\n' "$ID" "$1" > "$tmp/r/kama.json"
}
req ''
printf 'import { cam::ping };\nfn uint8 main()\n{\n    return cast<uint8>(ping());\n}\n' > "$tmp/r/src/main.kama"
"$KAMA" pkg install "$tmp/r/kama.json" > "$tmp/log" 2>&1 || fail "the app with a package could not install it"
if b "$tmp/r"; then fail "an app lacking what its package requires built"; fi
grep -qF 'the package `cam` requires `NSCameraUsageDescription`' "$tmp/log" || fail "the package's requirement was refused for another reason"
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>NSCameraUsageDescription</key><string>Scan</string></dict></plist>\n' > "$tmp/r/ios/Info.plist"
printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>com.apple.developer.game-center</key><true/></dict></plist>\n' > "$tmp/r/ios/game.entitlements"
req '"plist": "ios/Info.plist"'
if b "$tmp/r"; then fail "an app lacking the entitlement its package requires built"; fi
grep -qF "state it in the app's entitlements" "$tmp/log" || fail "the missing entitlement was refused for another reason"
req '"plist": "ios/Info.plist", "entitlements": "ios/game.entitlements"'
b "$tmp/r" || fail "an app stating what its package requires did not build"

# `assets`, at the bundle's root, beside the executable, where std::asset looks.
mkdir -p "$tmp/b/assets/levels" && printf '1' > "$tmp/b/assets/levels/one.json"
printf '{\n  "name": "hello",\n  "version": "1.2.3",\n  "kind": "app",\n  "entry": "src/main.kama",\n  "source": "src",\n  "assets": "assets",\n  "app": { %s },\n  "modules": { ".": { "visibility": "internal" } }\n}\n' "$ID" > "$tmp/b/kama.json"
b "$tmp/b" || fail "an app with assets did not build"
[ -f "$out/hello.app/assets/levels/one.json" ] || fail "the assets are not at the bundle's root"

echo "check-ios-app: PASS (A: the manifest's rules and what a build refuses before it compiles; B: the simulator's .app, its" \
     "plist and project, the app's own keys, both icon forms, the privacy manifest's check, a package's requirements, assets)"
