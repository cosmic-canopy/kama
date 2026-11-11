#!/bin/sh
# check-android-app.sh — `"kind": "app"` (KR-283): the manifest's `app` rules everywhere, and, where an Android NDK, SDK
# and JDK are installed, the APK `kama build` makes, read back by the SDK's own tools.
#
# A. Needs nothing but kama. An app states `app.id` (two or more segments, each a letter then letters and digits) and a
#    `version` of three numbers (no pre-release: `1.0.0-beta.2` would share `1.0.0`'s version code) whose code,
#    major×1000000 + minor×1000 + patch, fits; `app` belongs to an app, but for a library's `android.permissions`; a
#    keystore holds no password. On a target with no app format an app is an executable, so `kama run` runs it. `kama
#    run` of an Android app takes no arguments for `main`, and `--device` is refused on a run here.
# B. The APK, built for x86_64-linux-android29 as the SDK's emulator runs it: aapt2 reads back the id, the derived
#    version code, both API levels and the activity; the library is stored at lib/x86_64/, zipalign finds it on a 16 KB
#    page, apksigner verifies it (v2 included); `vr` writes Meta's entries, the OpenXR loader's and a network security
#    configuration; the APK carries the prebuilt libraries the program needs and the device lacks, and refuses what
#    would not load (found nowhere, another machine, a versioned SONAME, 4 KB pages at targetSdk 35); a supplied
#    manifest is checked where it must agree with kama.json; a debug APK is debuggable and a --release one is not;
#    --release signs with the project's key and its password from the environment; an app on
#    std::app's loop and std::app::android builds; with no $JAVA_HOME, the JDK a fake Android Studio lends packages it,
#    ahead of `jar` on PATH, and with no JDK at all the refusal names all three places. Running the APK needs a device,
#    so the emulator run is by hand (docs/targets.md); this holds what can be read without one. An app's `assets` are
#    packed under `assets/` with `/` in every name, media stored and the rest compressed.
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
# `kama run` of an Android app runs it on a device, which this guard has none of and must not start adb to look for
# (a server would outlive it): what is refused before adb is asked is held here, and the run itself by hand
# (docs/targets.md). The launcher passes no arguments, and `--device` means nothing to a run here.
if "$KAMA" run "$tmp/host/kama.json" --target x86_64-linux-android29 -- one > "$tmp/log" 2>&1; then
    fail "\`kama run\` of an Android app took arguments for \`main\`"
fi
grep -qF 'with no arguments' "$tmp/log" || fail "\`kama run\` of an Android app with arguments was refused for another reason"
if "$KAMA" run "$tmp/host/kama.json" --device emulator-5554 > "$tmp/log" 2>&1; then fail "\`--device\` was taken by a run here"; fi
grep -qF -- '--device names the Android device' "$tmp/log" || fail "\`--device\` on a run here was refused for another reason"

# ---- B ---------------------------------------------------------------------------------------------------------------
case "$(uname -s)" in
    MINGW*|MSYS*) exe=.exe; bat=.bat; sdk=${ANDROID_HOME:-${USERPROFILE:-}/AppData/Local/Android/sdk} ;;
    Darwin)       exe=;     bat=;     sdk=${ANDROID_HOME:-$HOME/Library/Android/sdk} ;;
    *)            exe=;     bat=;     sdk=${ANDROID_HOME:-$HOME/Android/Sdk} ;;
esac
command -v cygpath >/dev/null 2>&1 && sdk=$(cygpath -u "$sdk")
bt=$(ls -d "$sdk"/build-tools/*/ 2>/dev/null | while read -r d; do [ -f "$d/aapt2$exe" ] && echo "${d%/}"; done | sort -t/ -k1 -V | tail -1)
level=$(ls -d "$sdk"/platforms/android-*/ 2>/dev/null | sed 's|.*/android-||; s|/$||' | grep -E '^[0-9]+$' | sort -n | tail -1)
# The JDK this guard links a fake Android Studio to: $JAVA_HOME, else the one `jar` on PATH belongs to.
jhome=${JAVA_HOME:-}
command -v cygpath >/dev/null 2>&1 && [ -n "$jhome" ] && jhome=$(cygpath -u "$jhome")
if [ -z "$jhome" ] && command -v jar >/dev/null 2>&1; then jhome=$(CDPATH= cd -- "$(dirname -- "$(readlink -f "$(command -v jar)")")/.." && pwd); fi
java_ok=n; [ -n "$jhome" ] && [ -f "$jhome/bin/jar$exe" ] && java_ok=y
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

# The loop (KR-283 step 2): an app on std::app and std::app::android builds, so the glue's looper pump, the entry's
# globals and the accessors compile and link against the NDK under the app's define.
mkdir -p "$tmp/loop/src"; cp "$tmp/b/kama.json" "$tmp/loop/kama.json"
cat > "$tmp/loop/src/main.kama" <<'EOF'
import { std::app::App, std::app::Lifecycle, std::app::run, std::app::tickWhileSuspended,
         std::app::android::window, std::app::android::activity, std::app::android::setInputHandler,
         std::app::android::InputHandler };
unsafe fn bool onInput(UnsafePtr event) { return false; }
unsafe fn bool attach() { InputHandler h = onInput; setInputHandler(handler: h); return cast<usize>(activity()) != 0 && cast<usize>(window()) == 0; }
type resource Loop implements App {
    int32 n;
    public ctor make() { this.n = 0; }
    public fn bool tick() { this.n = this.n + 1; return this.n < 3; }
    public fn void lifecycle(Lifecycle event) { if (event == Lifecycle::Suspended) { tickWhileSuspended(on: false); } }
}
fn uint8 main() { bool attached = attach(); Owned<App> a = new Loop.make(); run(app: give a); return attached ? 0 : 1; }
EOF
apk "$tmp/loop" || fail "an app on std::app and std::app::android did not build for Android"
[ -f "$tmp/user/debug.keystore" ] || fail "the debug key was not made in \$ANDROID_USER_HOME"
"$bt/aapt2$exe" dump badging "$(native "$out/hello.apk")" > "$tmp/badging" 2>&1 || fail "aapt2 cannot read the APK"
for want in "package: name='com.example.kamahello' versionCode='1002003' versionName='1.2.3'" "minSdkVersion:'29'" \
            "targetSdkVersion:'$level'" "launchable-activity: name='android.app.NativeActivity'" \
            "application: label='Hello's app'" "native-code: 'x86_64'" "application-debuggable"; do
    grep -qF -- "$want" "$tmp/badging" || { cp "$tmp/badging" "$tmp/log"; fail "aapt2 does not read \"$want\" from the APK"; }
done
# zipalign reads both facts, with no `unzip` (which CI's Windows runner lacks): each entry's alignment, and whether it
# is stored — a compressed one prints `(OK - compressed)`.
"$bt/zipalign$exe" -c -v -P 16 4 "$(native "$out/hello.apk")" > "$tmp/log" 2>&1 || fail "zipalign does not find the APK 16 KB-aligned"
tr -d '\r' < "$tmp/log" | grep -qE ' lib/x86_64/libhello\.so \(OK\)$' \
    || fail "lib/x86_64/libhello.so is not stored uncompressed and aligned in the APK (extractNativeLibs=\"false\" needs it so)"
apksigner verify --min-sdk-version 24 -v "$(native "$out/hello.apk")" > "$tmp/log" 2>&1 || fail "apksigner does not verify the APK"
grep -qF 'Verified using v2 scheme (APK Signature Scheme v2): true' "$tmp/log" || fail "the APK carries no v2 signature"

# Assets (KR-283 step 3): `jar` packs the project's `assets` under `assets/`, every name `/`-separated (`aapt2 link -A`
# names a nested one `assets/levels\1.json` on Windows), stores the media aapt stores and compresses the rest, and an
# app reading them through std::asset builds against the asset manager.
app "$tmp/as" app '  "assets": "data",\n  "app": { "id": "com.example.assets", "android": { "targetSdk": '"$level"' } },\n'
mkdir -p "$tmp/as/data/levels" "$tmp/as/data/sounds"
printf '{"level":1}' > "$tmp/as/data/levels/1.json"
printf 'OggS' > "$tmp/as/data/sounds/boom.ogg"
yes 'text text text' | head -n 2000 > "$tmp/as/data/big.txt"
printf 'import { std::asset::readText };\nfn uint8 main()\n{\n    return match (readText(name: "levels/1.json")) { case Ok(value: s): 7; case Err(error: e): 1; };\n}\n' \
    > "$tmp/as/src/main.kama"
apk "$tmp/as" || fail "an app with assets did not build"
asapk="$tmp/as/out/x86_64-linux-android29/debug/hello.apk"
"$jhome/bin/jar$exe" tf "$(native "$asapk")" | tr -d '\r' > "$tmp/aslist" || fail "jar cannot list the app with assets"
for want in assets/levels/1.json assets/sounds/boom.ogg assets/big.txt; do
    grep -qx "$want" "$tmp/aslist" || { cp "$tmp/aslist" "$tmp/log"; fail "the APK has no entry $want"; }
done
"$bt/zipalign$exe" -c -v -P 16 4 "$(native "$asapk")" | tr -d '\r' > "$tmp/log" 2>&1 || fail "zipalign does not find the app with assets aligned"
grep -qE ' assets/sounds/boom\.ogg \(OK\)$' "$tmp/log" || fail "the .ogg asset is not stored uncompressed"
grep -qF ' assets/big.txt (OK - compressed)' "$tmp/log" || fail "the text asset is not compressed"

# `vr` writes Meta's entries for an immersive app, the OpenXR loader's own (step 4: its permissions and the `<queries>`
# for the runtime brokers, which Gradle merges from the loader's AAR), and names a network security configuration
# refusing cleartext, which aapt2 compiles into the APK.
app "$tmp/vr" app '  "app": { "id": "com.example.vr", "android": { "targetSdk": '"$level"', "vr": ["quest3", "quest3s"] } },\n'
apk "$tmp/vr" || fail "the vr app did not build"
for want in 'android.hardware.vr.headtracking' 'com.oculus.supportedDevices" android:value="quest3|quest3s"' \
            'com.oculus.intent.category.VR' 'android:excludeFromRecents="true"' 'org.khronos.openxr.permission.OPENXR"' \
            'org.khronos.openxr.permission.OPENXR_SYSTEM' \
            'android:authorities="org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker"' \
            'org.khronos.openxr.OpenXRRuntimeService' 'android:networkSecurityConfig="@xml/network_security_config"'; do
    grep -qF -- "$want" "$tmp/vr/out/x86_64-linux-android29/debug/AndroidManifest.xml" || fail "the vr manifest has no $want"
done
vrapk="$(native "$tmp/vr/out/x86_64-linux-android29/debug/hello.apk")"
"$bt/aapt2$exe" dump resources "$vrapk" | grep -qF 'xml/network_security_config' \
    || fail "the vr APK carries no xml/network_security_config resource"
"$bt/aapt2$exe" dump xmltree --file res/xml/network_security_config.xml "$vrapk" | grep -qF 'cleartextTrafficPermitted=false' \
    || fail "the vr APK's network security configuration does not refuse cleartext"

# Prebuilt libraries (KR-283 step 4, ruled 2026-11-08): the APK carries every library the program needs that the
# device lacks, found in the link's -L directories, and theirs in turn, each stored on a 16 KB page. Refused: one found
# nowhere (the build would crash at launch), one of another machine, a versioned SONAME (the installer extracts only
# lib*.so), and 4 KB pages at targetSdk 35, which Google Play refuses; below 35 those are a warning. The libraries are
# built with the NDK's own clang, as a vendor's would be.
ndk=${ANDROID_NDK_HOME:-$(ls -d "$sdk"/ndk/*/ | sed 's|/$||' | sort -t/ -k1 -V | tail -1)}
command -v cygpath >/dev/null 2>&1 && ndk=$(cygpath -u "$ndk")
ndkcc=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/clang$exe | head -1)
# solib <dir> <name> <soname> <C source> <target triple> [flags…] — a shared library <dir>/lib<name>.so.
solib() {
    d=$1; nm=$2; so=$3; src=$4; tg=$5; shift 5
    mkdir -p "$d"; printf '%s\n' "$src" > "$d/$nm.c"
    "$ndkcc" --target="$tg" -shared -fPIC -o "$(native "$d/lib$nm.so")" "$(native "$d/$nm.c")" -Wl,-soname,"$so" "$@" \
        > "$tmp/log" 2>&1 || fail "the NDK's clang could not build lib$nm.so"
}
# plapp <dir> <targetSdk> <ldflags, as JSON strings> <link, as JSON strings> — an app whose C calls use_libs().
plapp() {
    app "$1" app '  "ldflags": ['"$3"'],\n  "link": ['"$4"'],\n  "csources": [{ "path": "csrc/use.c" }],\n  "app": { "id": "com.example.prebuilt", "android": { "targetSdk": '"$2"' } },\n'
    mkdir -p "$1/csrc"; printf 'int use_libs(void);\n' > "$1/csrc/use.h"
    printf 'extern "use.h";\nextern fn int32 use_libs();\nunsafe fn int32 call() { return use_libs(); }\nfn uint8 main()\n{\n    return call() == 7 ? 7 : 1;\n}\n' > "$1/src/main.kama"
}
L() { printf '"-L%s"' "$(native "$1")"; }
x86=x86_64-linux-android29; page16=-Wl,-z,max-page-size=16384
solib "$tmp/v/bar" bar libbar.so 'int bar(void) { return 3; }' $x86 $page16
solib "$tmp/v/foo" foo libfoo.so 'int bar(void); int foo(void) { return bar() + 4; }' $x86 $page16 -L"$(native "$tmp/v/bar")" -lbar
plapp "$tmp/pl" "$level" "$(L "$tmp/v/foo"), $(L "$tmp/v/bar")" '"foo"'
printf 'int foo(void);\nint use_libs(void) { return foo(); }\n' > "$tmp/pl/csrc/use.c"
apk "$tmp/pl" || fail "an app linking a prebuilt library that needs another did not build"
"$bt/zipalign$exe" -c -v -P 16 4 "$(native "$tmp/pl/out/x86_64-linux-android29/debug/hello.apk")" | tr -d '\r' > "$tmp/pllist" 2>&1 \
    || { cp "$tmp/pllist" "$tmp/log"; fail "zipalign does not find the app with prebuilt libraries aligned"; }
for want in libfoo.so libbar.so; do
    grep -qE " lib/x86_64/$want \\(OK\\)\$" "$tmp/pllist" || { cp "$tmp/pllist" "$tmp/log"; fail "the APK does not carry lib/x86_64/$want stored on a 16 KB page"; }
done
grep -qE 'libc\.so|liblog\.so' "$tmp/pllist" && { cp "$tmp/pllist" "$tmp/log"; fail "the APK carries a library the device supplies"; }
# plrefused <name> <substring> <targetSdk> <ldflags> — the app links libfoo, and the build is refused saying <substring>.
plrefused() {
    plapp "$tmp/$1" "$3" "$4" '"foo"'; cp "$tmp/pl/csrc/use.c" "$tmp/$1/csrc/use.c"
    if apk "$tmp/$1"; then fail "$1: the APK was built"; fi
    grep -qF -- "$2" "$tmp/log" || fail "$1: the refusal does not say \"$2\""
}
plrefused plmissing 'libfoo.so needs libbar.so, which Android does not have at API level 29' "$level" "$(L "$tmp/v/foo")"
solib "$tmp/v/arm" bar libbar.so 'int bar(void) { return 3; }' aarch64-linux-android29 $page16
plrefused plmachine 'is a 64-bit aarch64 library, and this app is built for x86_64' "$level" "$(L "$tmp/v/foo"), $(L "$tmp/v/arm")"
solib "$tmp/v/bar4" bar libbar.so 'int bar(void) { return 3; }' $x86 -Wl,-z,max-page-size=4096
plrefused pl4k35 'an app targeting API 35 or later needs' 35 "$(L "$tmp/v/foo"), $(L "$tmp/v/bar4")"
# Below 35 it is a warning, said before the SDK is asked for anything, so it holds whatever platform is installed.
plapp "$tmp/pl4k" 34 "$(L "$tmp/v/foo"), $(L "$tmp/v/bar4")" '"foo"'; cp "$tmp/pl/csrc/use.c" "$tmp/pl4k/csrc/use.c"
apk "$tmp/pl4k" || true
grep -qF 'warning:' "$tmp/log" && grep -qF 'not 16 KB' "$tmp/log" || fail "a 4 KB library at targetSdk 34 was not warned about"
solib "$tmp/v/baz" baz libbaz.so.1 'int baz(void) { return 7; }' $x86 $page16
plapp "$tmp/plver" "$level" "$(L "$tmp/v/baz")" '"baz"'
printf 'int baz(void);\nint use_libs(void) { return baz(); }\n' > "$tmp/plver/csrc/use.c"
if apk "$tmp/plver"; then fail "an app needing a versioned SONAME was built"; fi
grep -qF 'needs libbaz.so.1, and an APK carries only libraries named lib' "$tmp/log" \
    || fail "an app needing a versioned SONAME was refused for another reason"

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
keytool="$jhome/bin/keytool$exe"
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
"$bt/aapt2$exe" dump badging "$(native "$tmp/rel/out/x86_64-linux-android29/release/hello.apk")" > "$tmp/log" 2>&1 \
    || fail "aapt2 cannot read the --release APK"
if grep -qF 'application-debuggable' "$tmp/log"; then fail "the --release APK is debuggable, which Play and Meta's store refuse"; fi

# The JDK (KR-283 step 3, ruled 2026-11-08): $JAVA_HOME, else Android Studio's `jbr/`, else `jar` on PATH. `studio
# <name>` installs a fake Android Studio under $tmp/<name>, at version 2099.1 so it is the newest, in the layout kama
# reads on this host, and prints where its JDK goes; `bystudio <name> <cmd…>` runs a command with no $JAVA_HOME that
# finds that one.
studio() {
    d="$tmp/$1"
    case "$(uname -s)" in
        MINGW*|MSYS*) mkdir -p "$d/local/Google/AndroidStudio2099.1" "$d/install"
                      printf '%s\r\n' "$(native "$d/install")" > "$d/local/Google/AndroidStudio2099.1/.home"
                      echo "$d/install/jbr" ;;
        Darwin)       c="$d/home/Applications/Android Studio.app/Contents"; mkdir -p "$c/jbr/Contents"
                      printf '<plist><dict><key>CFBundleShortVersionString</key><string>2099.1</string></dict></plist>\n' > "$c/Info.plist"
                      echo "$c/jbr/Contents/Home" ;;
        *)            mkdir -p "$d/home/.cache/Google/AndroidStudio2099.1" "$d/install"
                      printf '%s\n' "$d/install" > "$d/home/.cache/Google/AndroidStudio2099.1/.home"
                      echo "$d/install/jbr" ;;
    esac
}
bystudio() {
    d="$tmp/$1"; shift
    case "$(uname -s)" in
        MINGW*|MSYS*) env -u JAVA_HOME LOCALAPPDATA="$(native "$d/local")" "$@" ;;
        *)            env -u JAVA_HOME HOME="$d/home" ANDROID_HOME="$sdk" "$@" ;;
    esac
}
# Studio's JDK packages the app, and apksigner, which finds `java` through $JAVA_HOME or PATH alone, runs on it.
jbr=$(studio lend)
if [ -n "$bat" ]; then cmd //c "mklink /J $(cygpath -w "$jbr") $(cygpath -w "$jhome")" > /dev/null; else ln -s "$jhome" "$jbr"; fi \
    || fail "could not link the fake Android Studio's jbr/ to $jhome"
bystudio lend "$KAMA" build "$tmp/b/kama.json" --target x86_64-linux-android29 > "$tmp/log" 2>&1 \
    || fail "with no \$JAVA_HOME, the app did not build on Android Studio's JDK"
apksigner verify "$(native "$out/hello.apk")" > "$tmp/log" 2>&1 || fail "apksigner does not verify the APK signed on Android Studio's JDK"
# It comes before `jar` on PATH: a Studio whose `jar` exits 3 fails the build where a PATH one would have worked.
jbr=$(studio broken); mkdir -p "$jbr/bin" "$tmp/three/src"
printf 'fn uint8 main()\n{\n    return 3;\n}\n' > "$tmp/three/src/main.kama"
"$KAMA" build "$tmp/three/src/main.kama" -o "$jbr/bin/jar$exe" > "$tmp/log" 2>&1 || fail "could not build the stand-in jar"
if bystudio broken "$KAMA" build "$tmp/b/kama.json" --target x86_64-linux-android29 > "$tmp/log" 2>&1; then
    fail "a broken Android Studio JDK was passed over"
fi
grep -qF 'jar failed (exit 3)' "$tmp/log" || fail "the build with a broken Android Studio JDK failed for another reason"
# With none of the three, the refusal names them all. Only where `jar` is not on PATH, which the guard cannot take
# away without taking the shell's own tools with it, and with no Android Studio installed where kama would find one.
if ! command -v jar >/dev/null 2>&1 && ! ls -d "/Applications/Android Studio"*.app /opt/android-studio >/dev/null 2>&1; then
    mkdir -p "$tmp/none/local" "$tmp/none/home"
    if env -u JAVA_HOME LOCALAPPDATA="$(native "$tmp/none/local")" HOME="$tmp/none/home" ANDROID_HOME="$sdk" \
        "$KAMA" build "$tmp/b/kama.json" --target x86_64-linux-android29 > "$tmp/log" 2>&1; then
        fail "an app built with no JDK anywhere"
    fi
    for want in '$JAVA_HOME is not set' 'Android Studio' '`jar` is not on PATH'; do
        grep -qF -- "$want" "$tmp/log" || fail "the refusal for no JDK does not say \"$want\""
    done
fi

echo "check-android-app: PASS (A: the manifest's rules, and an app on the host; B: the APK at API $level, read back by aapt2," \
     "zipalign and apksigner; an app on std::app; vr; prebuilt libraries; a supplied manifest; --release signing; Android" \
     "Studio's JDK; assets)"
