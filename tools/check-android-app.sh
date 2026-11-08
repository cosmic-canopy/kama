#!/bin/sh
# check-android-app.sh — `"kind": "app"` (KR-283): the manifest's `app` rules everywhere, and, where an Android NDK, SDK
# and JDK are installed, the APK `kama build` makes, read back by the SDK's own tools.
#
# A. Needs nothing but kama. An app states `app.id` (two or more segments, each a letter then letters and digits) and a
#    `version` of three numbers (no pre-release: `1.0.0-beta.2` would share `1.0.0`'s version code) whose code,
#    major×1000000 + minor×1000 + patch, fits; `app` belongs to an app, but for a library's `android.permissions`; a
#    keystore holds no password. On a target with no app format an app is an executable, so `kama run` runs it.
# B. The APK, built for x86_64-linux-android29 as the SDK's emulator runs it: aapt2 reads back the id, the derived
#    version code, both API levels and the activity; the library is stored at lib/x86_64/, zipalign finds it on a 16 KB
#    page, apksigner verifies it (v2 included); `vr` writes Meta's entries; a supplied manifest is checked where it must
#    agree with kama.json; --release signs with the project's key and its password from the environment. Running the
#    APK needs a device, so the emulator run is by hand (docs/targets.md); this holds what can be read without one.
#    The debug key goes to a private $ANDROID_USER_HOME, never the real ~/.android.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
. "$ROOT/tools/kama-bin.sh"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
fail() { echo "check-android-app: FAIL — $1" >&2; [ -f "$tmp/log" ] && sed -n '1,30p' "$tmp/log" >&2; exit 1; }

# app <dir> <kind> <app json> [version] — a one-file project whose `main` returns 7.
app() {
    mkdir -p "$1/src"
    printf '{\n  "name": "hello",\n  "version": "%s",\n  "kind": "%s",\n  "entry": "src/main.kama",\n  "source": "src",\n%b  "modules": { ".": { "visibility": "internal" } }\n}\n' \
        "${4:-1.2.3}" "$2" "$3" > "$1/kama.json"
    printf 'fn uint8 main()\n{\n    return 7;\n}\n' > "$1/src/main.kama"
}
# refused <name> <substring> <kind> <app json> [version] — `kama check` refuses the manifest, saying <substring>.
refused() {
    app "$tmp/$1" "$3" "$4" "${5:-}"
    if "$KAMA" check "$tmp/$1/kama.json" > "$tmp/log" 2>&1; then fail "$1: \`kama check\` accepted it"; fi
    grep -qF -- "$2" "$tmp/log" || fail "$1: the refusal does not say \"$2\""
}

# ---- A ---------------------------------------------------------------------------------------------------------------
refused noapp     'app.id'                              app ''
refused shortid   'is not an app id'                    app '  "app": { "id": "game" },\n'
refused digitid   'is not an app id'                    app '  "app": { "id": "com.1game" },\n'
refused dashid    'is not an app id'                    app '  "app": { "id": "com.my-game" },\n'
refused prerel    '"1.0.1", through the store'          app '  "app": { "id": "com.example.game" },\n' 1.0.0-beta.2
refused build     'carries a build tag'                 app '  "app": { "id": "com.example.game" },\n' 1.0.0+7
refused minor     'stop at 999'                         app '  "app": { "id": "com.example.game" },\n' 1.1000.0
refused toobig    'stop at 999'                         app '  "app": { "id": "com.example.game" },\n' 2101.0.0
refused password  'KAMA_KEYSTORE_PASSWORD'              app '  "app": { "id": "com.example.game", "android": { "keystore": { "path": "k.jks", "alias": "a", "storePassword": "x" } } },\n'
refused unknown   'unknown key `minSdk` in `app.android`' app '  "app": { "id": "com.example.game", "android": { "minSdk": 29 } },\n'
refused permname  'is not a permission'                 app '  "app": { "id": "com.example.game", "android": { "permissions": ["INTERNET"] } },\n'
refused onexe     '"kind": "app"'                       executable '  "app": { "id": "com.example.game" },\n'
refused libid     'states only `android.permissions`'   library '  "app": { "id": "com.example.game" },\n'
app "$tmp/libperm" library '  "app": { "android": { "permissions": ["android.permission.INTERNET"] } },\n'
"$KAMA" check "$tmp/libperm/kama.json" > "$tmp/log" 2>&1 || fail "a library stating only \`app.android.permissions\` was refused"

# On the host an app is an executable: `kama run` runs it.
app "$tmp/host" app '  "app": { "id": "com.example.game" },\n'
st=0; "$KAMA" run "$tmp/host/kama.json" > "$tmp/log" 2>&1 || st=$?
[ "$st" = 7 ] || fail "\`kama run\` of an app on the host exited $st, not 7"
if "$KAMA" run "$tmp/host/kama.json" --target x86_64-linux-android29 > "$tmp/log" 2>&1; then fail "\`kama run\` of an Android app was not refused"; fi
grep -qF 'is an Android app' "$tmp/log" || fail "\`kama run\` of an Android app was refused for another reason"

# ---- B ---------------------------------------------------------------------------------------------------------------
case "$(uname -s)" in
    MINGW*|MSYS*) exe=.exe; bat=.bat; sdk=${ANDROID_HOME:-${USERPROFILE:-}/AppData/Local/Android/sdk} ;;
    Darwin)       exe=;     bat=;     sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk} ;;
    *)            exe=;     bat=;     sdk=${ANDROID_HOME:-$HOME/Android/Sdk} ;;
esac
command -v cygpath >/dev/null 2>&1 && sdk=$(cygpath -u "$sdk")
bt=$(ls -d "$sdk"/build-tools/*/ 2>/dev/null | while read -r d; do [ -f "$d/aapt2$exe" ] && echo "${d%/}"; done | sort -t/ -k1 -V | tail -1)
level=$(ls -d "$sdk"/platforms/android-*/ 2>/dev/null | sed 's|.*/android-||; s|/$||' | grep -E '^[0-9]+$' | sort -n | tail -1)
jhome=${JAVA_HOME:-}
command -v cygpath >/dev/null 2>&1 && [ -n "$jhome" ] && jhome=$(cygpath -u "$jhome")
if [ -n "$jhome" ]; then java_ok=$([ -f "$jhome/bin/jar$exe" ] && echo y || echo n); else java_ok=$(command -v jar >/dev/null 2>&1 && echo y || echo n); fi
ndk_ok=n; { [ -n "${ANDROID_NDK_HOME:-}" ] || ls -d "$sdk"/ndk/*/ >/dev/null 2>&1; } && ndk_ok=y
if [ -z "$bt" ] || [ -z "$level" ] || [ "$level" -lt 29 ] || [ "$java_ok" != y ] || [ "$ndk_ok" != y ]; then
    echo "check-android-app: PASS (A: the manifest's rules, and an app on the host; B skipped — no NDK, SDK build-tools," \
         "platform at API 29 or later, and JDK)"
    exit 0
fi

export ANDROID_USER_HOME; ANDROID_USER_HOME=$(kama_native_path "$tmp/user")
APP_JSON='  "app": { "id": "com.example.kamahello", "label": "Hello'"'"'s app", "android": { "targetSdk": '"$level"' } },\n'
apk() { "$KAMA" build "$1/kama.json" --target x86_64-linux-android29 ${2:-} > "$tmp/log" 2>&1; }
native() { kama_native_path "$1"; }
apksigner() { if [ -n "$bat" ]; then cmd //c "$(native "$bt/apksigner.bat")" "$@"; else "$bt/apksigner" "$@"; fi; }

app "$tmp/b" app "$APP_JSON"
apk "$tmp/b" || fail "the app did not build an APK for x86_64-linux-android29"
out="$tmp/b/out/x86_64-linux-android29/debug"
[ -f "$out/hello.apk" ] || fail "no hello.apk in $out"
[ -f "$out/lib/x86_64/libhello.so" ] || fail "the library is not kept beside the APK at lib/x86_64/libhello.so"
[ -f "$tmp/user/debug.keystore" ] || fail "the debug key was not made in \$ANDROID_USER_HOME"
"$bt/aapt2$exe" dump badging "$(native "$out/hello.apk")" > "$tmp/badging" 2>&1 || fail "aapt2 cannot read the APK"
for want in "package: name='com.example.kamahello' versionCode='1002003' versionName='1.2.3'" "minSdkVersion:'29'" \
            "targetSdkVersion:'$level'" "launchable-activity: name='android.app.NativeActivity'" \
            "application: label='Hello's app'" "native-code: 'x86_64'"; do
    grep -qF -- "$want" "$tmp/badging" || { cp "$tmp/badging" "$tmp/log"; fail "aapt2 does not read \"$want\" from the APK"; }
done
unzip -lv "$out/hello.apk" | grep -E ' Stored .* lib/x86_64/libhello\.so$' > /dev/null \
    || fail "lib/x86_64/libhello.so is not stored uncompressed in the APK (extractNativeLibs=\"false\" needs it so)"
"$bt/zipalign$exe" -c -P 16 4 "$(native "$out/hello.apk")" > "$tmp/log" 2>&1 || fail "zipalign does not find the APK 16 KB-aligned"
apksigner verify --min-sdk-version 24 -v "$(native "$out/hello.apk")" > "$tmp/log" 2>&1 || fail "apksigner does not verify the APK"
grep -qF 'Verified using v2 scheme (APK Signature Scheme v2): true' "$tmp/log" || fail "the APK carries no v2 signature"

# `vr` writes Meta's entries for an immersive app.
app "$tmp/vr" app '  "app": { "id": "com.example.vr", "android": { "targetSdk": '"$level"', "vr": ["quest3", "quest3s"] } },\n'
apk "$tmp/vr" || fail "the vr app did not build"
for want in 'android.hardware.vr.headtracking' 'com.oculus.supportedDevices" android:value="quest3|quest3s"' \
            'com.oculus.intent.category.VR' 'android:excludeFromRecents="true"'; do
    grep -qF -- "$want" "$tmp/vr/out/x86_64-linux-android29/debug/AndroidManifest.xml" || fail "the vr manifest has no $want"
done

# A supplied manifest: used as it stands where it agrees, refused where it does not.
mkdir -p "$tmp/own"; app "$tmp/own" app '  "app": { "id": "com.example.kamahello", "android": { "targetSdk": '"$level"', "manifest": "Manifest.xml" } },\n'
grep -v -E 'android:version(Code|Name)=' "$out/AndroidManifest.xml" > "$tmp/own/Manifest.xml"
apk "$tmp/own" || fail "a supplied manifest that agrees with kama.json was refused"
"$bt/aapt2$exe" dump badging "$(native "$tmp/own/out/x86_64-linux-android29/debug/hello.apk")" | grep -qF "versionCode='1002003'" \
    || fail "an APK from a supplied manifest does not carry kama.json's version code"
sed 's/package="com.example.kamahello"/package="com.example.other"/' "$tmp/own/Manifest.xml" > "$tmp/own/M2" && mv "$tmp/own/M2" "$tmp/own/Manifest.xml"
if apk "$tmp/own"; then fail "a supplied manifest naming another package was accepted"; fi
grep -qF 'names package "com.example.other"' "$tmp/log" || fail "a supplied manifest's package was refused for another reason"

# --release signs with the project's key, whose password comes from the environment and nowhere else.
keytool=keytool; [ -n "$jhome" ] && keytool="$jhome/bin/keytool$exe"
app "$tmp/rel" app '  "app": { "id": "com.example.kamahello", "android": { "targetSdk": '"$level"', "keystore": { "path": "release.p12", "alias": "upload" } } },\n'
"$keytool" -genkeypair -keystore "$(native "$tmp/rel/release.p12")" -storepass sekrit -alias upload -keypass sekrit \
    -keyalg RSA -keysize 2048 -validity 30 -dname "CN=kama check" -noprompt > "$tmp/log" 2>&1 || fail "keytool could not make a test key"
if env -u KAMA_KEYSTORE_PASSWORD "$KAMA" build "$tmp/rel/kama.json" --target x86_64-linux-android29 --release > "$tmp/log" 2>&1; then
    fail "a --release app built with no \$KAMA_KEYSTORE_PASSWORD"
fi
grep -qF 'KAMA_KEYSTORE_PASSWORD' "$tmp/log" || fail "a --release app with no password was refused for another reason"
KAMA_KEYSTORE_PASSWORD=sekrit apk "$tmp/rel" --release || fail "a --release app did not build with \$KAMA_KEYSTORE_PASSWORD set"
apksigner verify --print-certs "$(native "$tmp/rel/out/x86_64-linux-android29/release/hello.apk")" > "$tmp/log" 2>&1 \
    || fail "apksigner does not verify the --release APK"
grep -qF 'CN=kama check' "$tmp/log" || fail "the --release APK is not signed with the project's key"

echo "check-android-app: PASS (A: the manifest's rules, and an app on the host; B: the APK at API $level, read back by aapt2," \
     "zipalign and apksigner; vr; a supplied manifest; --release signing)"
