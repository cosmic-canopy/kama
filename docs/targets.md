# Targets & toolchain setup

How to build kama for the platform you want, and what you need installed to do it. The language-level
rules — target triples, single-select groups and the `@compileFor` flags they derive — are in
[SPEC.md](SPEC.md#conditional-compilation--compileforflag-).

## The short version

```sh
kama build app.kama                              # this machine (the default)
kama build app.kama --release                    # optimized, stripped
kama build app.kama --target WASM                # the web
kama run app.kama --target WASM                  # ...run in a browser
kama build app.kama --target LINUX               # cross-compile
kama build lib.kama --select OUTPUT=STATIC       # a static library
kama build app.kama -j 4                         # cap concurrent C compiles (default: core count)
```

**Build parallelism.** A program that imports from `std` compiles several translation units — an import
brings in the files defining the names it asks for, plus their closure (see
[SPEC.md](SPEC.md#modules-); `examples/httpd` is 10) — and `kama build` compiles them
concurrently, `-j`/`--jobs` wide, defaulting to your core count. `$KAMA_BUILD_JOBS` sets the default;
`-j` on the command line beats it. It is purely a scheduling knob: the objects a build produces are
byte-identical at every width, which [`tools/check-build-jobs.sh`](../tools/check-build-jobs.sh) asserts.

Two cases stay a single compiler invocation, deliberately: `--release` native (which folds the whole
program into one translation unit so the C compiler can inline across modules), and a **bundled**
install, whose `zig cc` keeps its own object cache — one invocation re-compiles only what changed
(~0.1 s), which beats anything splitting them could do.

**Both tiers write `#line`**, so an error only the C compiler can find, such as an `extern const` the header does not
define or an extern type's layout, names the kama line in a release build as it does in a debug one
([`tools/check-release-line.sh`](../tools/check-release-line.sh)). It reaches no byte of a release binary, which
carries no debug info and reads no `__FILE__`: the fixture corpus and the benchmarks build byte-identical with and
without it. `--no-line` leaves it out.

kama compiles to **ISO C11** and hands it to a C compiler. That is why cross-compiling is realistic:
the hard part is not code generation, it's having a C compiler that can reach the target. Everything
below is about that.

## Built-in targets

| `--target` | Triple | Needs |
|---|---|---|
| `HOST` *(default)* | this machine's | your system C compiler — already there |
| `MACOS` | `<arch>-macos-none` | the macOS SDK (free on a Mac; awkward elsewhere) |
| `WINDOWS` | `<arch>-windows-gnu` | a mingw-w64 toolchain, or zig |
| `LINUX` | `<arch>-linux-gnu` | a Linux sysroot, or zig |
| `WASM` | `wasm32-emscripten-none` | [Emscripten](https://emscripten.org) (`emcc` on PATH) |
| `EMBEDDED` | `<arch>-none-none` | nothing extra for the host's own arch |

A **named target follows the host's architecture** — `--target LINUX` on an Apple Silicon Mac means
`aarch64-linux-gnu`. When the arch matters, spell the triple: `--target x86_64-linux-gnu`. Names are
conveniences; triples are exact. A build names one target, and only an Android app takes several: its ABIs, packaged
together ([Android apps](#android-apps)).

Anything containing a `-` is taken as a bare `<arch>-<os>-<abi>` triple and needs no configuration at
all, which is how you reach a target kama has never heard of:

```sh
kama build firmware.kama --target thumbv7em-none-eabihf --cc "arm-none-eabi-gcc"
kama build app.kama      --target riscv64-linux-musl
```

Four components are `<arch>-<vendor>-<os>-<abi>`, and the vendor is dropped. The three-part spelling clang
and gcc often use puts a vendor in the middle (`aarch64-apple-darwin`, `x86_64-w64-mingw32`), and kama
refuses it rather than misreading it: a middle `apple`, `pc`, `unknown` or `w64` is an error naming kama's
spelling (`aarch64-macos-none`, `x86_64-windows-gnu`). The architecture is `aarch64`, never `arm64`.

**Android** is `<arch>-linux-android<level>`, clang's spelling, the API level the abi's suffix:
`aarch64-linux-android29` (Meta Quest's ABI, at its lowest level), or `x86_64-linux-android29` for the SDK's
emulator. It derives `OS_LINUX` and `ABI_ANDROID`, and no flag per level: bionic runs std's Linux code, and an
Android difference is a declaration gated on `ABI_ANDROID`. An unversioned `aarch64-linux-android` is refused, as
the NDK's own headers would refuse it. With no `cc`, kama finds the NDK's clang itself:

1. `$ANDROID_NDK_HOME`, an NDK root;
2. else the newest `ndk/<version>` (by number) of the SDK at `$ANDROID_HOME`;
3. else of Android Studio's default SDK: `%USERPROFILE%\AppData\Local\Android\sdk` on Windows,
   `~/Library/Android/sdk` on macOS, `~/Android/Sdk` elsewhere.

The first that is set is the answer, never a fallback past a wrong one, and a refusal names where it looked. The
NDK's `llvm-ar` archives a static library. A target's `cc` (an NDK wrapper, say) still wins. Bionic keeps pthreads
in libc, so nothing links `-lpthread`. C++ `csources` link the NDK's libc++ in statically, since no device
carries `libc++_shared.so`; `--dynamic-runtime` keeps it shared, for you to ship beside the program (an app's APK
carries it, as it carries any library the device lacks). Every Android
link aligns the program's segments to 16 KB pages, which a device with them requires. A program built this way runs
under `adb shell` with a stdout; an app the launcher starts is the next section.

```sh
kama build app.kama --target aarch64-linux-android29
adb push app /data/local/tmp/ && adb shell /data/local/tmp/app
```

**iOS** is `<arch>-ios<minimum>-<abi>`, clang's spelling, the minimum on the os: `aarch64-ios17.0-none` for a device,
`aarch64-ios17.0-simulator` for the simulator on an Apple Silicon Mac, `x86_64-ios17.0-simulator` on an Intel one
(KR-281 decision 2). It derives `OS_IOS`, `ABI_NONE` or `ABI_SIMULATOR`, `HOSTED`, `SIMD128` and `DARWIN`, the Apple
family that macOS derives too, and no flag per version. An unversioned `aarch64-ios-none` is refused naming the
versioned spelling: handed no minimum, Apple's clang writes iOS 7.0 into the binary without a word (14.0 for the
simulator), a floor nobody chose. With no `cc`, kama finds Apple's clang and the SDK itself, as Xcode's build does
(`xcrun --sdk iphoneos` or `iphonesimulator`, `--find clang` and `--show-sdk-path`), and hands clang
`arm64-apple-ios17.0[-simulator]` and `-isysroot`. That needs Xcode itself, selected with `xcode-select`: the Command
Line Tools carry no iOS SDK. A machine without it still analyzes the program and reports its errors first, as for any
target whose toolchain is missing (no NDK, no libc for a cross triple), and refuses the build only once the program
is sound. A program built for the simulator runs under `xcrun simctl spawn booted <program>`, with
a stdout; an iOS app, with UIKit's loop, is KR-281's next step. An iOS target does not derive `CHILD_PROCESSES`, since
the sandbox refuses `fork` and `posix_spawn` (SPEC, *Subprocesses*), and `homeDirectory()`,
`temporaryDirectory()`, `configDirectory()` and `cacheDirectory()` answer the app's container, its `tmp/`, and its
`Library/Application Support` and `Library/Caches`.

```sh
kama build app.kama --target aarch64-ios17.0-simulator
xcrun simctl spawn booted ./app
```

### Android apps

An app the launcher starts is a project whose kind is `app`. Its `app` block carries the identity a store knows it by,
which iOS will share, and how it is packaged for Android. `kama seed --kind app --id com.example.game` writes one whose
`main` runs on `std::app`'s loop, with `targetSdk` the newest platform the SDK has (else 34):

```json
{
  "name": "game",
  "version": "1.2.3",
  "kind": "app",
  "entry": "src/main.kama",
  "app": {
    "id": "com.example.game",
    "label": "Game",
    "android": { "targetSdk": 34 }
  },
  "modules": {
    ".": { "visibility": "internal" }
  }
}
```

```sh
kama run kama.json --target x86_64-linux-android29       # on the SDK's emulator; aarch64-linux-android29 for a phone or Quest
kama build kama.json --target aarch64-linux-android29    # just the APK, at out/aarch64-linux-android29/debug/game.apk
kama build kama.json --target aarch64-linux-android29 --target x86_64-linux-android29   # one APK holding both ABIs
```

- **`id`** is required and has no default, since a store knows the app by it for good: two or more segments split by
  `.`, each a letter and then letters and digits (what both Android and Apple accept). **`label`**, what the launcher
  shows, defaults to `name`.
- **The version is `version`**, three numbers with no pre-release or build tag. Android's version code is derived
  from it, `major*1000000 + minor*1000 + patch` (`1.2.3` is `1002003`), so the minor and patch parts stop at 999; a
  `1.0.0-beta.2` is refused, since it would share `1.0.0`'s code, which a store refuses on the second upload. Ship a
  beta as a patch, through the store's own test channel.
- **minSdk is the target's API level** (`android29` is 29). **`android.targetSdk` is required**: Android changes how an
  app behaves by it, so it is the project's choice (Meta's store requires 34), and it names the platform the manifest
  is compiled against.
- **`android.permissions`** lists the permissions the app asks for. A library may state this key, and only this one,
  in an `app` block of its own: the package that binds an API knows the permission it needs, and a build asks for every
  dependency's.
- **`android.vr`** is a Meta Quest app's: the headsets it supports, Meta's `supportedDevices`, such as
  `["quest3", "quest3s"]`. It writes three groups of entries:
  - what Meta's store requires of an immersive app: head tracking required, the VR launch category, landscape, out of
    recents;
  - the OpenXR loader's own two permissions and the `<queries>` for its runtime brokers, which Gradle merges from the
    loader's AAR into every Quest app;
  - a network security configuration refusing cleartext, `res/xml/network_security_config.xml`, which `aapt2
    compile` compiles. Meta's VRC.Quest.Packaging.4 asks for one without naming it; this is the shape Meta's Unity
    tooling writes, and the same policy Android applies by default since targetSdk 28.

  A Quest 3 on Horizon OS needs none of the loader's entries today, and is unaffected by them. It answers neither
  broker (both are registered to a `Disabled…Broker`), so the loader finds Meta's runtime through
  `/odm/etc/openxr/1/active_runtime.aarch64.json`. A runtime reached through a broker would need them.
- **`android.manifest`** names an `AndroidManifest.xml` of the project's own, which replaces the one kama writes, used
  as it stands. kama checks it only where the two must agree: the package is `app.id`, the activity is
  `android.app.NativeActivity` with `android.app.lib_name` naming the program, any SDK level stated is the build's,
  and it states no version (kama writes the version from `version`). The one kama writes is left beside the APK, as a
  start for your own.
- **`android.res`** names a resource directory of the project's own, in Android's layout (`values/`,
  `values-fr/`, `mipmap-anydpi-v26/`, `xml/`, …), which `aapt2 compile` compiles as it stands, as Gradle compiles
  `src/main/res`. So anything Android's resources can say works without kama knowing it: strings and their
  translations, an adaptive icon, a network security configuration. Where it has `xml/network_security_config.xml`,
  a `vr` app ships that file rather than kama's, since aapt2 refuses two. Only the root project may state it, as with
  `assets`: a library's native code reaches a compiled resource only through JNI.
- **`android.icon`** is the launcher icon, as a resource reference such as `"@mipmap/ic_launcher"`, which the manifest
  kama writes carries as `android:icon`. It is named, never found by convention. An adaptive icon (API 26 and later)
  is round and square at once, so there is no `roundIcon`. A project with its own `android.manifest` names the icon
  there, and stating both is refused. One image for every platform is decided with the iOS target.

  ```json
  "android": { "targetSdk": 34, "res": "res", "icon": "@mipmap/ic_launcher" }
  ```
- **`android.keystore`** — `{ "path": "…", "alias": "…" }` — is the key a `--release` build signs with, which is
  refused without it. Its passwords are read from `$KAMA_KEYSTORE_PASSWORD`, and `$KAMA_KEY_PASSWORD` when the key's
  differs, and never from `kama.json`. A debug build signs with `~/.android/debug.keystore` (or
  `$ANDROID_USER_HOME/debug.keystore`), Android Studio's debug key, which kama makes as Android Studio does when it is
  missing, so a debug APK from either installs over the other.
- **A debug APK is debuggable** (`aapt2 link --debug-mode`), as Gradle's `debug` build type makes one: a debugger, the
  profiler in app mode (`simpleperf --app`) and `adb shell run-as <id>` reach it. A `--release` APK never is, since
  Play and Meta's store refuse an upload that is.
- **`assets`** — a top-level key on any project, not under `app` — names the directory of files the program reads
  through `std::asset` (SPEC § *Application loop*). In an app they are packed into the APK's `assets/` by the JDK's
  `jar`, media formats stored as aapt would store them and the rest compressed. Not by `aapt2 link -A`: on Windows it
  names a nested file `assets/levels\1.json`, which the device's asset manager never finds. Natively `kama build`
  copies them beside the executable, and on the web they are preloaded at `/assets`. The build records every name in
  the directory, leaving out hidden files, and refuses a name that holds `\`, or two names that differ only in case,
  since Windows' and macOS's file systems keep those as one file. The directory is the root's alone: a dependency
  naming `assets` is refused (ruled 2026-11-08).

**Prebuilt libraries travel in the APK** (ruled 2026-11-08). A library the program links that the device does not have
is stored at `lib/<abi>/` beside the program, on a 16 KB page. Without it, the app installs and then dies at launch:
`UnsatisfiedLinkError … library "libopenxr_loader.so" not found` (measured on a Quest 3).
- **Which libraries.** kama reads what the linked program needs (its `DT_NEEDED`), and then what each of those needs.
  A library the NDK has a stub for at the target's API level (`libc`, `liblog`, `libEGL`, `libvulkan`, …) is the
  device's.
- **Where kama looks** for any other: the `-L` directories the build passed, in order, then the NDK's own (its
  `libc++_shared.so`). One found nowhere refuses the build, naming it and the library that needs it.
- **What kama checks**, as Android does. Each library must be:
  - an ELF of the ABI's machine;
  - named `lib*.so` and calling itself that (its `SONAME`), since the installer extracts nothing else and the program
    asks for it by that name;
  - on 16 KB pages (its `LOAD` segments) when `targetSdk` is 35 or more, which Google Play requires. Below 35 a smaller
    alignment is a warning: a Quest's pages are 4 KB, Meta asks nothing, and a vendor's library may not be
    rebuildable.

The OpenXR loader every Quest app needs is one such library. Meta has required Khronos's loader since 2024-04-30 and
supports 1.0.34 and later. Unpack `org.khronos.openxr:openxr_loader_for_android` from Maven Central (an AAR is a zip)
and name its parts:

```json
"cflags":  ["-I<aar>/prefab/modules/headers/include"],
"ldflags": ["-L<aar>/jni/arm64-v8a"],
"link":    ["openxr_loader", "EGL", "GLESv3"]
```

A library a program opens by name with `dlopen` is in no `DT_NEEDED`, and is not carried yet (KR-350).

**What it takes.** The NDK, found as above, and from the SDK (`$ANDROID_HOME`, else Android Studio's default) the newest
build-tools and the platform `targetSdk` names; a missing one is refused with the `sdkmanager` line that installs it.
And a JDK, for the `jar` and `keytool` kama runs and the `java` that `apksigner` runs on, looked for in this order:
1. `$JAVA_HOME`.
2. The JDK Android Studio bundles, its `jbr/` (Android Studio 2022 and later), from the newest install. On Windows and
   Linux an install records where it lives in the `.home` file of its `Google/AndroidStudio<version>` directory
   (`%LOCALAPPDATA%` on Windows, `~/.cache` on Linux); Linux's `/opt/android-studio` and `~/android-studio` count too.
   On macOS it is an `Android Studio*.app` in `/Applications` or `~/Applications`. kama hands it to `apksigner` as
   `$JAVA_HOME`.
3. `jar` and `keytool` on PATH.

Android Studio's comes before PATH because macOS keeps a stub `jar` on PATH even when no JDK is installed. With none
of the three, the build is refused naming them all. kama runs the platform's own
tools and writes only text: the manifest, and a `vr` app's network security configuration, which `aapt2 compile`
compiles. Then:
- `aapt2` links the manifest into an APK holding no code (the activity is the platform's `NativeActivity`);
- the JDK's `jar` adds the libraries uncompressed;
- `zipalign -P 16` puts each library on a 16 KB page;
- `apksigner` signs it: v2 and v3, and v4's `.idsig` beside the APK, which `adb install` uses to install incrementally.

The libraries stay beside the APK, at `lib/<abi>/`, for a symbolizer.

**Several ABIs** (ruled 2026-11-11). A `--target` per ABI makes one package holding each, the APK and the App Bundle
alike, as Gradle's and Flutter's packages do by default, and the device takes its own ABI. The set is the build's, not the project's,
because the stores differ:
- Play requires a 64-bit ABI for each 32-bit one, and x86_64 reaches Chromebooks and Play Games on PC.
- Meta's store takes arm64-v8a alone.

So one project ships to both. The rules:
- The targets name one API level, since an app has one minSdk, and each ABI once.
- One `--target` is one ABI, and kama adds none.
- Two targets for anything but an Android app are refused.
- Each target builds as it would alone, in a process of its own (its C is its own), into its own
  `out/<triple>/<type>/lib/<abi>/`. It carries the libraries its own build finds, so an `-L` gated by `ARCH_…` reaches one ABI.
- The package is assembled once, in the first target's directory, or where `-o` names it, with every ABI's `lib/` beside it.
- One APK per ABI is not made: a bundle answers it, since Play splits one per device.

**And a notice.** The `native_app_glue` compiled into every app is the Android Open Source Project's code under the
Apache License 2.0 (its `NOTICE` sits beside it in the NDK, `sources/android/native_app_glue/`), which asks that a
copy of the licence travel with the app, as an app's open-source-licences screen does. Nothing else of the NDK's is
compiled in: bionic and the platform's libraries are the device's.

**How the program runs.** NativeActivity loads the library, and the NDK's `native_app_glue`, compiled into every app,
starts a thread at `android_main`, which kama's runtime supplies: it runs the program's `main` there, once per process.
stdout and stderr go to logcat under the tag `kama`, a line at a time, a `csources` file's `printf` included, and a
panic's message is in the crash report too (logcat's `Abort message:`). `args()` holds the app's id. When `main`
returns, the activity finishes and the process exits, so the next launch starts a fresh one rather than running `main`
again over the last run's globals.

**The app's directories** (ruled 2026-11-11). Zygote starts an app's process with no `HOME`, a `TMPDIR` only from
Android 13, and `/` as its directory, where nothing can be written. So before `main`, as .NET for Android's host does, the
runtime:
- sets `HOME` to the app's files directory (`getFilesDir()`);
- sets `TMPDIR` and `XDG_CACHE_HOME` to its cache directory (`getCacheDir()`, asked of the activity through JNI as
  gomobile and SDL3 ask it), which Android clears under storage pressure, setting each only where it is unset;
- starts `main` in the files directory, so a relative path means the app's own storage.

std's `homeDirectory()`, `temporaryDirectory()`, `configDirectory()` and `cacheDirectory()` then answer the app's own
directories (`files`, `cache`, `files/.config`, `cache`), and a `TempDir` is made in the cache.

**The activity's lifecycle reaches the program through `std::app`** (SPEC, *Application loop*). `run(app:, pace:)` pumps the
glue's looper between ticks and calls the app's `lifecycle` as each change happens: `Resumed` and `Suspended` with the
activity, `SurfaceReady` when its window exists and `SurfaceLost` before it goes (drop every surface made from it in
that call: Android waits for it, then destroys the window), `FocusGained` and `FocusLost` (the notification shade), and
`LowMemory`. Rotation is none of these: the manifest kama writes claims every configuration change, so the window
stays and is resized in place. While suspended the loop does not tick and waits, using no CPU, unless the program has
said `tickWhileSuspended(on: true)`, as an OpenXR session must until it ends. An app on demand (`Pace::OnDemand`) waits
in the looper between ticks too, until one of these arrives:
- an input event;
- a change the app hears (each of the above);
- the window's own news (a new size, a new content rect, a redraw the OS asks for, a configuration change);
- a call to `ALooper_wake` from its `Waker`;
- a requested tick's due time, the poll's timeout.

The glue's own bookkeeping wakes nothing: the input queue attached, the activity started or stopped, its state saved.
`Pace::AtMost(hz:)` is the same timeout to the next deadline. So an activity's start ticks an app on demand at `Resumed`,
at `SurfaceReady` and the window's sizing, and at `FocusGained`, the last of it (measured on the emulator, `0.9.963`). The loop's own fibers
run between ticks at no pace here yet (SPEC, **Fibers beside the loop**; KR-411). Back ends the activity, and `run` returns
after `Suspended` and `SurfaceLost` have been heard. `std::app::android` is what a package needs of the activity:
`window()` (the `ANativeWindow`), `activity()` (the `ANativeActivity`: its `JavaVM`, activity object, asset manager
and data paths) and `setInputHandler(handler:)`, which is given each `AInputEvent`.

**On a Quest** (measured on a Quest 3, Meta's runtime `Oculus 209.174.0`), Horizon OS's changes reach an app as these
same events:

| what the user does | `Lifecycle` |
|---|---|
| open and close the universal menu | `FocusLost`; `FocusGained` |
| take the headset off | `FocusLost` at once, then `Suspended` and `SurfaceLost` when it sleeps, about 15 s later |
| put it back on | `Resumed`, `SurfaceReady` |
| Quit from the menu | `Suspended` and `SurfaceLost`, then `run` returns |

- Each resume brings a `FocusGained` and a `FocusLost` in the same pump, before the real `FocusGained`. So an XR program
  renders by its OpenXR session's state, which does not flicker.
- It keeps `tickWhileSuspended(on: true)` while the session runs, since the session's `STOPPING`, which
  `xrEndSession` must answer, arrives while the activity is suspended.
- Quit ends the activity before the session reaches `EXITING`. So `run` returns, and the app's drop destroys the
  session.

Android waits on the loop: it starts, pauses and destroys a window only once the loop has answered, and reports the app
as not responding after about five seconds of input it has not. So keep the work before `run` short (Android's start
waits for it), and each tick short; load in pieces across ticks, or on a thread. A `main` that never calls `run` is a
program that runs and finishes, answering nothing while it runs. An `OUTPUT` named outright still builds what it names:
`--select OUTPUT=EXE` builds the same program for `adb shell`.

**An App Bundle for Google Play** (ruled 2026-11-11). Google Play requires a new app as an Android App Bundle, and
`--select OUTPUT=AAB` builds one in place of the APK, which stays the default (Meta's store, sideloading and
`kama run` take an APK):

```sh
kama build kama.json --target aarch64-linux-android29 --release --select OUTPUT=AAB   # out/…/release/game.aab
```

- **The same parts, laid out as a bundle, by the platform's tools.** `aapt2 link --proto-format` writes the manifest
  and resource table as protocol buffers. `jar` zips them with the libraries and assets as the base module. Google's
  `bundletool build-bundle` builds the bundle, and the JDK's `jarsigner` signs it, since `apksigner` signs APKs only.
- **bundletool is found, never fetched.** The SDK does not include it. `$BUNDLETOOL` names its jar, which runs on the
  JDK found above; else a `bundletool` on PATH (Homebrew's `brew install bundletool`). With neither, the build is
  refused, naming where Google publishes it: `bundletool-all-<version>.jar` at
  [github.com/google/bundletool/releases](https://github.com/google/bundletool/releases).
- **Signed as its APK would be.** A debug build uses the debug key and is debuggable; Play refuses it, as it refuses a
  debug APK. A `--release` build uses `android.keystore`, the upload key under Play App Signing. Its passwords are read
  from the environment, as `apksigner`'s are.
- **Its ABIs are the build's targets**, one `--target` each (*Several ABIs*, above). bundletool splits the bundle per
  ABI, as Play serves it. An arm64-only bundle meets Play's 64-bit rule.
- **bundletool makes the APKs a device gets** from it, and stores every asset uncompressed, where kama's APK
  compresses all but media. That costs download size only.
- **A bundle is built, never run:** `kama run` refuses `OUTPUT=AAB`. To try one on a device, make and install the APKs
  Play would serve it:

  ```sh
  java -jar "$BUNDLETOOL" build-apks --bundle=game.aab --output=game.apks --connected-device
  java -jar "$BUNDLETOOL" install-apks --apks=game.apks
  ```

**`kama run` on a device** (ruled 2026-11-08). With an Android `--target`, `kama run` builds the APK as `kama build`
does, then on a device:
1. **It checks the device**, before anything builds. The device is adb's choice: `--device <serial>` is passed as adb's
   `-s`; without it, `$ANDROID_SERIAL` picks one, or the one attached is used. With several attached and neither, the
   run is refused, listing their serials. The device must list one of the targets' ABIs and run at least their API level, or
   the run is refused, naming the triple that would run there. An x86_64 emulator also lists `arm64-v8a`, through its
   ARM translator.
2. **It installs the APK** with `adb install -r --no-incremental`. An incremental install can still be arriving when
   the app starts. adb is the SDK's `platform-tools/adb`, else `adb` on PATH. A device that refuses the APK (signed
   with another key, or a newer version installed) is reported with the `adb uninstall <id>` that clears it. kama never
   uninstalls, since that deletes the app's data.
3. **It starts the app fresh,** with `am start -S -W`: `main` runs once per process, so a copy already running would
   run no new `main`.
4. **It prints the program's lines** as the program prints them (the `kama` tag of its process, without logcat's
   prefix), until the process ends.
5. **It exits with the status Android recorded** for the process, from API 30 on: `main`'s return, 128 plus the signal
   for a crash (134 for a panic), and 1 for any other end. API 29 keeps no such record, and the run says so.

Ctrl-C stops the app and exits 130. The launcher gives `main` no arguments, so `-- <args>` is refused.

**A Quest is a device like any other** once it is in developer mode (a Meta developer organisation, then the Meta
Horizon app). Connect it by USB-C and accept "Allow USB debugging?" inside the headset; the dialog that offers this
computer's files is a different permission. `kama run kama.json --target aarch64-linux-android29` then runs the app on
it. Work over adb goes on while the headset sleeps on a desk.

## Getting a cross toolchain

Three routes. **Try them in this order** — the first one that describes you is the least work.

### 1. You already have a cross toolchain

Use it. Declare it once in `kama.json` so it is checked in and the whole team gets it:

```json
{
  "link": ["m"],
  "select": {
    "TARGET": {
      "RPI": {
        "triple":  "aarch64-linux-gnu",
        "cc":      "aarch64-linux-gnu-gcc",
        "ar":      "aarch64-linux-gnu-ar",
        "sysroot": "/opt/rpi-sysroot",
        "cflags":  ["-mcpu=cortex-a72"],
        "ldflags": [],
        "runtime": "static",
        "link":    ["m", "atomic"]
      }
    }
  }
}
```

```sh
kama build app.kama --target RPI
```

`ar` matters for `OUTPUT=STATIC`: an archive indexed by the host's `ar` may be unreadable to the
target's linker.

**`link`** names native libraries by their bare name — `["m"]` becomes `-lm` — and an entry may be gated
on a fact, `{ "name": "dl", "compileFor": ["OS_LINUX"] }` (below). It is the portable half
of linking, which is why it sits on the *project* as well as on a target: a project that needs `-lm`
everywhere says so once, at the top level, instead of repeating it per target or making every caller
remember `--link m`. `ldflags` remains the raw-linker-text escape hatch for anything that is not a
library name.

A target's `link` **replaces** the project's rather than adding to it — that is what makes it possible
to say *"not that one, here"*. This is deliberately the opposite of `cflags`/`ldflags`, which append
onto the built-in target they merge over. A target that never mentions `link` inherits the project's
list unchanged. The CLI `--link` is unaffected and still appends, after both.

**Each entry is one argument to the C compiler, exactly as written, on every host** — `cflags`, `ldflags`, <!-- test: cflags_one_argument -->
`cxxflags`, `objcflags` and `link` alike. `"-DCFG=\"my_config.h\""` reaches the compiler with its quotes (the
Mbed TLS `MBEDTLS_CONFIG_FILE` idiom), a value with spaces stays one value, and two arguments are two entries
(`"-framework", "Metal"`). Until `0.9.473` the entries were pasted into a shell command, so a manifest carried shell
syntax — `\"` to keep a quote, `\$ORIGIN` to keep a dollar — which cmd.exe reads differently; an entry that still
escapes `"`, `$`, `'` or `` ` `` with a backslash is refused, naming the spelling that works. On Windows the
command passes through cmd.exe, and each of its metacharacters is escaped for it, so `&`, `|` and `%PATH%` are text
there too (until `0.9.478` a quote inside a value let cmd split the command at the next `&`).

**`cflags` and `ldflags` sit on the project too**, exactly like `link`:

```json
{
  "cflags":  ["-DFEATURE_X"],
  "ldflags": ["-Wl,--as-needed"],
  "select": { "TARGET": { "RPI": { "cflags": ["-mcpu=cortex-a72"] } } }
}
```

A project-level entry applies to **every** target; a target's list is **appended after** it, so the
command line reads project-then-target and the more specific list gets the last word wherever the C
compiler resolves a repeated flag last-wins. The honest limit of appending: a project-level flag cannot
be *removed* on one target the way `link` can be — override it with a later flag instead.

Two reasons it is on the project and not only on a target. A flag that is true of the artifact
(`-DFEATURE_X`) is not a fact about any one target, and saying it once beats repeating it in each.
More decisively: a target is matched **by name**, so `--target aarch64-linux-gnu` — an anonymous
triple — matches no `select.TARGET` entry at all, and a **dependency** cannot know how its consumer
spells the target. Without a project tier, a dependency's build settings could not reach such a build
at all; see [What a dependency contributes](#what-a-dependency-contributes).

### `no-heap`, `webgpu` and `reproducible-float` — the same shape, for the same reason

Three more project properties a target may override, spelled exactly like `link`:

```json
{
  "kind": "executable",
  "no-heap": true,
  "select": { "TARGET": { "HOST": { "no-heap": false } } }
}
```

**`no-heap`** forbids heap allocation program-wide, the same rule `--no-heap` applies. It wants a
manifest key more than either of the others do, because the FLAG fails **silently** when forgotten: the
build simply succeeds with allocation allowed, and a bare-metal target quietly gains a heap nobody asked
for. Saying it once in the manifest makes it a property of the project instead of something every
invocation has to remember.

What the rule proves is that the program never REACHES an allocation. `main`, every `expose fn` and every
`@foreignEntry` body are the roots (SPEC *No-heap subset*). It does not prove that the object never NAMES
`malloc`: a stdlib body that came in with an import and is never called may still reference it. So every no-heap
and bare-metal compile gets `-ffunction-sections -fdata-sections`, in debug as well as release, and **the board's
link step should pass `--gc-sections`**, which drops the unreached code. Measured on tests/support/embedded_board/src/main.kama
(a board clock, `sleep`, a collection and `sqrt`) built for `thumbv7m-none-eabi` with no sysroot: linked with no C library
and `--gc-sections` it boots and runs on QEMU (tools/check-mcu.sh), while a link without `--gc-sections` fails on the
allocation funnel's `malloc`, which no body it reaches calls. (Until KR-341 this paragraph cited a `std::uuid` object
measured with the host's own C headers present, which a board's compiler does not have; SPEC *Building* has what a
bare-metal build reads.)

**`webgpu`** is the `--webgpu` flag as a project property. It is also a *linking* decision, which is the
class `link` exists for. It is the platform API and nothing more: natively it finds the wgpu-native SDK
(`tools/fetch-webgpu.sh` puts it in `third_party/wgpu/<os>-<arch>/`, one per platform as `out/` is; or `$KAMA_WGPU_DIR`) and adds its include path, library and rpath; on wasm it selects
emscripten's WebGPU port and links as C++ (`-sDEFAULT_TO_CXX`), since the port's releases since Dawn v20260423 are
C++ inside and refuse a C link; a manifest's `emSettings` may still set it. A window, an event loop or a safe binding is the project's — a platform layer in its own
`csources`, linked by its manifest, as [examples/webgpu](../examples/webgpu/kama.json) does, or a package.

The per-target override is the ordinary case for both, not an exotic one: no heap on the board and a
heap on the host that builds your tooling; WebGPU from the browser on wasm and from the wgpu-native SDK
natively. Both values are checked when the manifest is read, so `"no-heap": "yes"` is refused by name
rather than read as some truthiness nobody wrote down. `--no-heap` and `--webgpu` can only turn a
setting **on**, so they OR in with the manifest; a target is how you say *"not this one"*.

**`reproducible-float`** is the third of the shape, and it says: *this program's floating point must
give the same bits on every target.*

```json
{ "reproducible-float": true }
```

Without it, clang's default lets `a * b + c` contract into a single fused multiply-add — one rounding
step instead of two. A target with an FMA (arm64) then gives different bits from one without
(wasm32 MVP, baseline x86-64 SSE2), so the same source disagrees with itself browser-versus-native.
Measured on this repo's own fixture: **13 of 64 random triples differ** on aarch64-macos in a release
build, and **0** with the key. Contraction only differs in a `--release` build: kama's own
overflow-checking arithmetic already splits a debug build's expressions.

**The key's other half is `std::math`.** `sqrt`, `floor`, `ceil`, `round`, `trunc`, `abs` and `fmod` are
exact, so every libm gives the same bits. IEEE 754 does not require `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `exp`, `log`, `log2`, `log10`, `cbrt`, `pow`, `atan2` or `hypot` to be correctly rounded, and each
platform's libm rounds them its own way in the last bit: over 5.8 million inputs, Windows' and
emscripten's disagree on 343,547, Windows disagrees with itself between a CPU with FMA3 and one without,
and a release wasm build disagrees with a debug one. So under the key those 14, in both widths, come from
musl (1.2.6) compiled into your program, the same code on every target, wasm included, in debug and
release alike. `tests/repro_math.d` holds their bits on every leg kama tests. Without the key they are
the platform's libm, exactly as fast as C's.

**What it costs.** Contraction off gives up the fused multiply-adds a target could have used. The math
runs at musl's speed instead of the platform's. On x86-64 Windows against UCRT (a quiet machine, ns per
call), the 28 functions sum to about the same, unevenly: musl's `log10` takes about twice as long (2.4
times in `float32`), `powf` 1.8 times and `tan` 1.4 times, while its `cbrt` runs twice as fast and
`hypotf` nearly four times. On wasm it was musl already, except that a release build now gets Arm's
table-driven `pow`, `log` and `log2` instead of emscripten's small ones, which costs about 12 KB more of
wasm in a program that calls all three (natively the small ones run `pow` 2.7 times slower; on wasm,
unmeasured).

**And its notices.** Those functions are code compiled into your program, so a program built with the
key carries their notices, and so does a bare-metal program that calls `std::math`, which compiles the
same header in (it has no C library to call). There are three, and each asks to travel with copies:
- **musl's MIT licence**, the first comment of `include/kama_libm.h`;
- **Sun Microsystems' notice**, above the functions that came from fdlibm, in three forms: two from 1993
  (SunPro and SunSoft) and one from 2004, each ending "provided that this notice is preserved";
- **Arm's MIT notice** (`SPDX-License-Identifier: MIT`), above the functions from Arm's optimized
  routines, in two forms (2018 and 2017–2018).

Each stands verbatim above its code in that header, which is where a program's third-party notices
take them from. A hosted program built without the key compiles none of this code in.

It is named for the goal rather than for what it emits today, because `-ffp-contract=off` and a C libm
are C-backend details and the goal is not. It is a boolean because kama can honor exactly two states, and
"absent" already means *"I have not thought about it"* — a third spelling for that would be a second
way to say the same thing. Manifest-only, deliberately: a property that correctness depends on must not
be losable by forgetting a command-line flag.

⚠️ One honest limit. The key applies to a **build**, so it does not travel through `kama transpile`
into someone else's toolchain: compile that C with `-ffp-contract=off -DKAMA_REPRODUCIBLE_FLOAT`
yourself.

**And one way it is NOT like the other two: it propagates from a dependency.** `no-heap` and `webgpu`
are read from your manifest only, because a dependency must not change what compiles in your program
or make your build demand an SDK. `reproducible-float` states a requirement of the dependency's own
arithmetic and of the math it calls, both of which run in your program. So a dependency that declares
it gets it, in your build, for the whole program: contraction off everywhere, and `std::math` from musl,
at the cost above. It is one-way: a dependency's `false` takes nothing from a consumer (or a sibling)
that asked. This was not true of `0.9.169` as shipped, where the key was grouped with the
other two — and a raw `-ffp-contract=off` in a dependency's `cflags` propagated while the first-class
key replacing it did not.

`runtime` is `"static"` or `"dynamic"` and says how the *language's own* runtime is linked — see
[Runtime linkage](#runtime-linkage) below. Every key is optional; omitting one takes the target's
default.

If the paths are personal (your own sysroot location), put the target in **`kama.local.json`**
instead — same shape, gitignored, overrides the committed manifest.

### A build variant — flags on a `select` value

A sanitizer build is not a target — it is *this* target, built another way — so its flags live on a build
type:

```json
"select": { "BUILD_TYPE": {
    "ASAN": { "inherits": "DEBUG",
              "cflags":  ["-fsanitize=address,undefined", "-fno-sanitize=function", "-fno-omit-frame-pointer"],
              "ldflags": ["-fsanitize=address,undefined"] } } }
```

```sh
kama build kama.json --select BUILD_TYPE=ASAN
kama build kama.json --select BUILD_TYPE=ASAN --target LINUX
```

`-fno-sanitize=function` is not optional under `undefined`: a contract call goes through a slot typed with a `void*`
receiver, which that one check reports on every call (SPEC *Building & debugging* gives the reason; the test suite
passes the same flag).

Any `select` value takes `cflags`, `ldflags`, `cxxflags` and `objcflags`, meaning what they mean on a target
entry. The chosen value's flags come after the project's and the target's, base first along `inherits` (an
`ASAN2` inheriting `ASAN` gets `ASAN`'s, then its own), and they reach **every** translation unit, a
dependency's C included — a sanitizer that missed the C it links would miss the bug. A `TARGET` entry needs
a `triple` unless it names a built-in it adjusts, and `kama check` refuses one that has neither, as `kama
build --target` does.

### `release` — what a release build optimizes for

`--release` compiles and links every target at `-O3`, wasm included. The root manifest may ask for size instead,
and name packages or modules built at the other level:

```json
{ "release": { "optimize": "size", "speed": ["image", "app::physics"] } }
```

- **`optimize`** is `"speed"` (`-O3`, the default) or `"size"` (`-Oz`). A target entry takes it too, so one manifest
  ships a small page and a fast native build: `"select": { "TARGET": { "WASM": { "release": { "optimize": "size" } } } }`.
  A size variant is a target of its own, chosen with `--target`.
- **`speed`** and **`size`** name packages or modules compiled at that level whatever the build's own is: a codec's
  loops at speed in a size-first page. Entries are spelled as `debug.optimize`'s are. One that names nothing in the
  build is refused by name, so is an entry under both, and between a package and a module inside it the module wins.
- **A release build is still one translation unit**, so std inlines into the code that calls it. When levels mix, that
  unit compiles at `-O3`, and every function of the code at size is marked `minsize`. That is what `-Oz` is to clang:
  the `-O2` pipeline with `minsize` on every function. A `csources` file is its own unit and simply takes its level on
  its line.
  - **Measured:** a size build with its own package at speed came out 1.2 KB of brotli over plain `-Oz`, with
    `-O3`'s speed in that package.
- **On wasm the link's level picks emscripten's libc.** A size build links `libc_optz`, whose `pow`, `log` and `log2`
  are table-free and slower, and a package at speed inside it still calls those. Measured on 2026-11-15
  (`docs/benchmarks/wasm-opt-*.tsv`, made by `bench/wasm-opt/run.sh`):
  - **Speed:** `-Oz` takes 2.2 to 11 times as long as `-O3`, in node and in three browsers. kama's checked operations
    count on inlining, and `-Oz` keeps them as calls.
  - **Size:** `-O3` costs 0.4 to 11 KB of brotli. Most of the 11 is libc's tables.
- **Only the root manifest's applies**, as with `debug`. A dependency's `release` applies when it is the root, and it
  never sets its consumer's level: a constant-time C file keeps its guarantees with barriers in its code, which
  hold at every level, not with a level (BoringSSL's approach).
- **An optimization level in a flag key is refused**: `-O2` in `cflags`, `cxxflags`, `objcflags` or `ldflags`,
  anywhere, names these keys instead. In `cflags` it used to sit before kama's own level and do nothing. In a wasm
  build's `ldflags` it set the whole program's level from any package in the graph. <!-- xfail: flag_optimize_level -->
- **`closure`** is `true` unless said: a wasm release runs Closure (`--closure=1`) over its JS. On `examples/webgpu`
  (emscripten 6.0.13) it took the JS from 15,461 to 8,707 bytes of brotli and the whole download from 27.4 KB to 20.7,
  for about 2 s of link. `false` turns it off, and a target entry takes it too. A debug build never runs it, and a
  native build has no JS. What it asks of a project's own JS is under [`jsLibraries`](#jslibraries-and-emsettings--the-emscripten-pair).
- **`--closure` in a flag key is refused**, as a level is: Closure runs over the whole program, so a package's flag
  would decide it for every consumer. `--closure-args`, which hands Closure a library's externs, is not refused.
  <!-- xfail: flag_closure -->

The guard is `tools/check-release-optimize.sh`.

### `debug.optimize` — a package at the release level in a debug build

A debug build compiles at `-O0`, and some code cannot run there: a measured DSP took 1.36 ms a frame in debug
against 14.8 µs in release, about 95 times slower, so a game's audio cannot keep real time while the rest of it
is being debugged. The root manifest names the packages, or modules, a debug build compiles at the level a release
build gives them (`release`, above), as Cargo's `[profile.dev.package.<name>] opt-level` does:

```json
{ "debug": { "optimize": ["voice", "std::math"] } }
```

An entry is a package (`voice`, `std`), a module (`voice::dsp`), or a module with everything inside it. Only the
optimization level changes. `-g` stays, so the code can still be stepped through, and `-DNDEBUG` is not added.
kama's overflow traps, bounds checks and asserts are in the C it emits, not in `-O`, so they stay as well. A
package's `csources` follow its package. An entry that names nothing in the build is refused by name
(`tools/check-debug-optimize.sh`).

**Only the root manifest's applies.** How a program is debugged is the program's choice, so a dependency cannot
choose it for its consumers. A library's own `debug` applies when the library is the root, for its own tests.
`--release` already optimizes everything, and the key changes nothing there. It is a level for whole packages:
a level per function is a non-goal (ROADMAP_DETAIL §2, *Verdicts*).

### `csources` — your own C, C++, Objective-C and Swift, compiled by `kama build`

A project (or a package) can hand the build its own sources:

```json
{
  "csources":  ["csrc/adder.c", "csrc/ui.cpp", "csrc/window.m"],
  "cxxflags":  ["-fno-exceptions"],
  "objcflags": ["-fobjc-arc"]
}
```

Each entry is one translation unit, linked into the same artifact — no out-of-band Makefile. **Its
language is its extension**, the way cgo, CMake and the cc crate read it:

| extension | language | standard | flags it gets |
|---|---|---|---|
| `.c` | C | `-std=gnu11` | `cflags` |
| `.cpp`, `.cc`, `.cxx` | C++ | `-std=gnu++17` | `cflags`, then `cxxflags` |
| `.m` | Objective-C | `-std=gnu11` | `cflags`, then `objcflags` |
| `.mm` | Objective-C++ | `-std=gnu++17` | `cflags`, then `cxxflags`, then `objcflags` |
| `.swift` | Swift, an Apple target only | `-swift-version 6` | none (below) |

kama's own generated C stays ISO `c11`. A `.c` entry is somebody else's C, and the C the world writes is
GNU C (emscripten's `EM_ASM`, which libsodium's entropy source uses, refuses to compile in a `-std=c*`
mode). C++ is pinned to `gnu++17` — what clang and GCC 11+ already default to — so a compiler upgrade
cannot change your build; say `"cxxflags": ["-std=c++20"]` to move it, and since `cxxflags` come after
the pinned standard, yours wins. Each entry compiles as its own command, so its standard never reaches
another translation unit.

`cxxflags` and `objcflags` sit on the project and in a `select.TARGET` arm, and a dependency's reach
its consumer, exactly like `cflags` — with one difference that is the reason they exist: `cflags` reach
**every** translation unit, kama's own C included, while `cxxflags` reach only the C++ entries and
`objcflags` only the Objective-C ones. The C-only warning promotions kama adds for its own C
(`-Werror=incompatible-pointer-types` and friends) stay off C++.

**Your compiler's family decides two of those promotions, and kama asks it which family it is** (one
`-v` per build, because `cc` is gcc on most Linux and clang on macOS, and a cross prefix or a
wrapper can be either). `-Werror=return-type` and `-Werror=uninitialized` are promoted everywhere. The
pointer class is not, and cannot be: handing a kama `fnptr` to a C callback field is the sanctioned FFI
seam — a kama callback lowers enums to `int` and handles to `void*`, which C sees as an incompatible
function pointer. **clang** names that case separately, so kama promotes every incompatible pointer to an
error and demotes the function-pointer one back to a warning. **gcc** has one name for both, so promoting
it would reject the seam itself; kama leaves the pointer class a warning there, and kama's own refusal of a
`const`-to-mutable pointer conversion — which happens before any C compiler sees the program — carries it. A build with
gcc is therefore slightly laxer about C pointer mismatches and identical about everything else.

**The C++ driver brings the C++ runtime.** A build holding a C++ or Objective-C++ entry compiles those
entries, and **links**, with the C++ spelling of your C compiler — `clang` → `clang++`, `gcc` → `g++`,
`cc` → `c++`, `zig cc` → `zig c++`, `emcc` → `em++`, keeping any prefix or version suffix
(`aarch64-linux-gnu-gcc` → `aarch64-linux-gnu-g++`, `clang-17` → `clang++-17`). That driver links the
right runtime on every target (`libc++` on macOS, `libstdc++` on Linux, zig's bundled libc++, emscripten's),
so there is no `-lc++`/`-lstdc++` to name per target. (On wasm the link stays with `emcc` — `em++`
cannot take it, because the same invocation compiles kama's own C — and kama passes `-sDEFAULT_TO_CXX` so
that link still binds the C++ runtime. `emcc` does not do it on its own: without that setting every
`std::string` and `__cxa_throw` is undefined at `wasm-ld`.) A build **without** a C++ entry never uses it, so
a program that shares a package tree with a C++ UI does not link a C++ runtime. When your C compiler has
no C++ spelling kama knows, name one: `--cxx <compiler>` beside `--cc`, or `"cxx"` beside a target's
`"cc"`; otherwise the build is refused by name.

Paths are **relative to the manifest that declares them**, and each entry's directory goes on the
include path, so a header sitting beside the source is found both from that source and from the kama
file that `extern "adder.h";`s it. A C++ header shared with kama wants the usual `extern "C"` guard.

A dependency's `csources` are compiled too, which is what lets a package that wraps a C library ship
the shim that binds it. Two packages that both ship `csrc/shim.c` are fine — the object is named after
the package that owns the source — and so is `foo.c` beside `foo.cpp`.

Two smaller rules, both refusals with a message: an **absolute** path (a manifest must stay
relocatable, and a published package cannot name a directory nobody else has) and a path escaping the
project with `..`. A path that simply is not there is named by kama, not by the C compiler.

`OUTPUT=OBJECT` builds one translation unit by definition, so it refuses a build that has a `csources`
entry beside the program — `OUTPUT=STATIC` is the shape that takes several.

**A `.swift` entry** (KR-281 decision 6e) is for what only Swift reaches on Apple's platforms: StoreKit's purchase API
(the one it replaced is "No longer supported" from iOS 18.0), SwiftUI, App Intents, ActivityKit. A package's Swift is
one module, as SwiftPM compiles a target: every `.swift` entry it declares goes to one invocation of Xcode's `swiftc`
for the target's SDK (`xcrun --sdk macosx|iphoneos|iphonesimulator swiftc -parse-as-library -swift-version 6 -wmo`,
`-O` in a release build), so its files may call each other, and it is named after the package (`@kama/store` is
`kama_store`). It exports what kama calls with `@c` (Swift 6.3, SE-0495), and kama binds that as it binds any C
function, through a header beside it:

```swift
// store.swift
import StoreKit
@c(store_purchase)
public func purchase(_ id: UnsafePointer<CChar>, _ done: @convention(c) (Int32) -> Void) { … }
```

```kama fragment
extern "store.h";                     // void store_purchase(const char* id, void (*done)(int32_t status));
extern fn void store_purchase(UnsafeConstPtr<cchar> id, PurchaseDone done);
```

Its `-target` is the C's: an iOS triple's, or on macOS `$MACOSX_DEPLOYMENT_TARGET` when set (swiftc does not read the
variable itself) and the running OS's major version otherwise, as clang uses. No link flag follows: the object names
the OS's Swift runtime itself, and Apple's linker reads that, so nothing is embedded. The SDK's compiled modules are
cached in `.kama-cache/swift-modules` beside the build's output, and the object in the build cache, so a warm build
compiles no Swift. On any other target a `.swift` entry is refused by name, since no API outside Apple's is reachable <!-- xfail: csources_swift_off_apple -->
only from Swift: gate it `{ "path": "store.swift", "compileFor": ["DARWIN"] }`, and give the other targets their own <!-- test: csources_swift -->
C (`compileFor: ["!DARWIN"]`).

### A source for some targets only — `compileFor` on an entry

A `csources` or `cincludes` entry can be an object carrying a gate:

```json
{
  "csources": [
    "src/ui.cpp",
    { "path": "src/platform_mac.m", "compileFor": ["OS_MACOS"] },
    { "path": "src/audio_native.c", "compileFor": ["!ARCH_WASM32"] }
  ]
}
```

`compileFor` holds exactly the literals `@compileFor(...)` takes — flag names and `!FLAG`, all of which
must hold — and is judged by the same rule, over the same flags: the derived `ARCH_`/`OS_`/`ABI_`/`HOSTED`/`SIMD128`/`CHILD_PROCESSES`/`FILE_WATCH`
facts, `DEBUG`/`RELEASE`, a declared `select` value, a `flags` entry. So gate on the fact, never on how
the build was spelled: `OS_MACOS` holds for a `HOST` build on a Mac, for `--target MACOS` and for a bare
`aarch64-macos-none`, while a `select.TARGET.MACOS` arm would match only the second. That is also why a
**dependency** can ship a macOS-only source: it cannot know what its consumer calls the target, but it
can state the fact, and its gate is judged against the build it compiles into.

The refusals are the `@compileFor` ones — a name that is neither a build-configuration fact nor declared,
and a gate no configuration can activate (`["DEBUG", "RELEASE"]`) — plus one of the manifest's own: an
entry whose gate leaves it **out** of this build must still exist, the way a gated-out kama file is
still parsed. A path that is wrong only on the platform nobody builds daily is exactly what a gate would
otherwise hide.

**A `link` entry and a dependency take the same gate.** `"link": ["m", { "name": "dl", "compileFor":
["OS_LINUX"] }]` links `-ldl` only where the fact holds — so a package keeps a native library out of a wasm
build by the fact (`"!ARCH_WASM32"`), which no consumer's spelling of its wasm target can defeat. A
dependency edge carries one beside its source — `"audio": { "path": "../audio", "compileFor":
["!ARCH_WASM32"] }` — and a dependency is in the build when an *active* edge reaches it from the project, so
one gated out takes along the dependencies only it needed. A gated-out dependency is still installed (the
lock is the same on every platform), and its modules can still be imported: gate the code that uses them, as
the code calling a gated `csources` entry is gated. What it would add to the C command line — sources,
include paths, libraries, flags — stays out.

**So does a build-flag entry**, in `cflags`, `ldflags`, `cxxflags` and `objcflags` alike, wherever the list sits —
the project, a dependency, a target entry, a `select` value (`0.9.651`):

```json
{
  "link":    [{ "name": "glfw", "compileFor": ["OS_MACOS"] }],
  "ldflags": [{ "flag": "-L/opt/homebrew/lib", "compileFor": ["OS_MACOS"] },
              { "flag": "-Wl,-framework,Cocoa", "compileFor": ["OS_MACOS"] }]
}
```

That is how a platform layer says what macOS alone needs: a Homebrew library directory, a framework. A
`select.TARGET` arm cannot say it, because it matches how the build was spelled, and a plain build on a Mac
spells no target. A gated-out argument leaves the line, the rest keep the order they were written in, and an
entry stays one argument (`-Wl,-framework,Cocoa`, not `-framework` and `Cocoa`, so one gate covers it).

### `cincludes` — a package's include tree

A header beside its `.c` is found on its own (above). A library whose headers live in their own tree —
which is most of them — names the tree:

```json
{
  "csources":  ["third_party/foo/src/foo.c"],
  "cincludes": ["third_party/foo/include"]
}
```

Each entry is a directory, **relative to the manifest that declares it**, put on the include path for
every translation unit of the build — the package's own C and the kama file that `extern "foo/foo.h";`s
it alike. A dependency's `cincludes` reach its consumer's build the way its `csources` do, which is what
lets a package vendor a C library whole. The same three refusals as `csources` (absolute, escaping with
`..`, not there — a missing directory is named by kama, with the package that declared it, rather than
surfacing as a missing header three steps later). This is the structured route for what a raw `-I` in a
dependency's `cflags` cannot say, and why that is refused below.

### A package carries sources, not binaries

**A package cannot ship a prebuilt archive, and that is a decision (2026-09-18), not a gap.** A relative `-L` in a
dependency's `ldflags` is refused — it would resolve against the consumer's working directory — and `link` names a
library without saying where it is. Whether a package ecosystem carries binaries splits by whether its users have
a C toolchain. Where they do not, it is standard: Python wheels, npm's per-platform packages, NuGet's
`runtimes/<rid>/`. Where a compiler is guaranteed, it is an anti-pattern: Cargo crates are source and a `-sys`
crate builds its C or finds a system library, Go modules are source, Zig repackages C libraries with a source
build. The objections are the same each time — a binary cannot be audited or reproduced, it is per-target AND
per-toolchain (a mingw `.a`, an MSVC `.lib`, a libc++ and a libstdc++ build are four artifacts for one platform),
and it goes stale against the ABI it was built for. kama always has a C compiler, and ships one.

What answers the need instead:

- **Build from source with `csources`.** `@kama/sodium` lists libsodium's 120 files rather than carrying
  `libsodium.a`; a manifest that size builds on every host, Windows included.
- **For what cannot be built that way** — a cmake project, a closed vendor SDK — the PACKAGE says what it needs
  (`link`) and the ROOT project says where it is (its own `ldflags`). The root is the only party that knows the
  machine, so this is the arrangement, not a workaround.

### `jsLibraries` and `emSettings` — the emscripten pair

```json
{
  "jsLibraries": ["js/audio_glue.js"],
  "emSettings": {
    "EXPORTED_RUNTIME_METHODS": ["ccall", "cwrap"],
    "STACK_SIZE": "4MB",
    "ALLOW_MEMORY_GROWTH": true
  }
}
```

**`jsLibraries`** is a plain list of `--js-library` files, resolved against the manifest that declares
them. emcc accumulates them, so there is nothing to resolve between a project and its dependencies —
a package that binds a browser API ships the glue that binds it.

**A release build runs Closure over every one of them** ([`release`](#release--what-a-release-build-optimizes-for)),
and Closure renames each property name it cannot prove belongs to something outside the program:

- **A name the browser defines** (`binaryType`, `createDataChannel`, `serverCertificateHashes`) is safe as written:
  Closure ships declarations for those.
- **A name something outside the build reads** is written quoted, on both sides: a JSON message's keys
  (`{ 'seq': n }`), a member of `Module` (`Module['_kama_app_wake']`), a field the page's own script reads.
- **A name only the file uses** is left for Closure to shorten.

A mistake does not fail the build. It is a property that reads `undefined` at run time, so a library is tested in a
release build. The consumer's `"release": { "closure": false }` is the way out while a library is fixed. std's own
glue and `kama run`'s page script follow this rule: the wasm leg runs its browser fixtures at `--release`.

emcc's other file inputs have a key each, resolved the same way:

```json
{
  "shellFile": "web/shell.html",
  "embedFiles": ["assets"],
  "preloadFiles": ["data/level.bin@/levels/1.bin"]
}
```

**`shellFile`** is the `--shell-file` page around the program. It is the project's alone: one page holds
a program, so a dependency naming one is refused. **`embedFiles`** and **`preloadFiles`** pack files into
the program's virtual filesystem (`--embed-file`, `--preload-file`), from the project or from a
dependency. An entry is `src` or emcc's own `src@dst`. Without `@dst`, the file lands where its relative
spelling puts it (`assets` → `/assets`), which is where emcc put it when run from the manifest's directory.
As raw `ldflags` these resolved against wherever the build was started, so they built from the manifest's
own directory only.

A **`select.TARGET` entry takes all five keys**, so one wasm configuration can carry its own page, assets and
glue. Its lists follow the project's, as its `ldflags` do; it has the last word on `shellFile` and on an
`emSettings` scalar, and an `emSettings` list unions with the project's.

**`emSettings`** is an object, and the **shape of each value decides what it means**:

| value | kind | how it merges |
|---|---|---|
| an array of strings | **list** | **unions** — kama's values, then dependencies', then yours |
| a string, number or boolean | **scalar** | last-wins, and **you win** over kama and over every dependency |

That is the point of the key. emcc is last-wins on a repeated `-s`, and the settings kama emits for you
(`EXIT_RUNTIME`, and `EXPORTED_RUNTIME_METHODS` when your program uses `std::net::web`, and the pthread
settings when it uses isolates) reached the command line *after* anything you smuggled in through
`cflags` — so the only way to say this was also the way to lose. Now
`"EXPORTED_RUNTIME_METHODS": ["ccall"]` gives you `ccall` **and** the two names the stdlib's own glue
needs.

Two dependencies setting the same scalar to different values is **refused, naming both** — and stating
that setting in your own `emSettings` settles it, because you merge last.

**A setting emcc reads at compile time reaches every C source too**: `WASM_WORKERS`, `SHARED_MEMORY`, `MEMORY64`,
`SUPPORT_LONGJMP`, the exception settings, a port's `USE_*` (emscripten's `COMPILE_TIME_SETTINGS` and
`PORTS_SETTINGS`). A `csources` file compiles on its own, so `"emSettings": {"WASM_WORKERS": 1}`, which an
AudioWorklet's C seam needs, shares the memory for that file's atomics and thread-locals as well as for kama's own
C. Up to `0.9.941` it reached only the link, and the link refused the file's object. Every other setting is a link
setting and goes to the link alone.

Both keys are **inert on a non-wasm target**, not an error: one manifest builds every target, and
`subsystem` set that precedent. The *shape* is still validated everywhere, so a typo is caught by
whoever wrote it rather than by whoever ships to the web.

⚠️ You can override `EXIT_RUNTIME`, and it is load-bearing: without it an emscripten program returns
from `main` and leaves node to wind the runtime down, which has been observed to deadlock against V8's
own background threads. Turn it off only if you know why you need to. Without it, a browser run (below)
never hears the program end, either.

### `kama run` in a browser

A wasm build is a web page, and `kama run kama.json --target WASM` runs it in a browser, as `kama run` runs a native
program here and an Android app on a device:
1. **It finds node and a browser**, before anything builds. The node is the one Emscripten runs: `$EM_NODE_JS`, else
   `$EMSDK_NODE` (emsdk's environment script sets it), else `node` on PATH. The browser is `$KAMA_BROWSER` (a path, or
   a program on PATH), else the first of Chrome, Edge, Chromium and Firefox found at their standard install places and
   then on PATH. Safari is never chosen: it has no headless mode and takes no profile on its command line. With none
   found, the run is refused, naming every place kama looked.
2. **It builds into a directory of its own**, in the temp directory, with a script linked in (`--pre-js`) that reports
   the program's output and its end. A project's `out/` is untouched, so it never holds a page that reports to a
   runner. A project's `shellFile` is the page, and keeps its own `Module` hooks beside the run's. With none, the page
   is a bare one of kama's own, a canvas filling the window under emscripten's name for it (`#canvas`): the output is
   the terminal's, and emscripten's default page (which `kama build` still writes) copies every line into a textarea, at
   a cost that grows with all the text before it.
3. **It serves the build on `127.0.0.1`**, on a free port, under a path no other page can guess. A build whose memory
   is shared (a thread form, or `emSettings` that share it) is served with the `Cross-Origin-Opener-Policy` and
   `Cross-Origin-Embedder-Policy` headers its deployment must send. Any other build goes without them, as its
   deployment would, since `require-corp` refuses a cross-origin font or script the page loads.
4. **It opens the page in a fresh profile**, removed after, so the user's own profile, extensions and windows stay out
   of it. The window shows by default. `--headless` opens none, for tests and CI, and the page has what a visible one
   has: Firefox, which starts no GPU process headless unless told to and so gave a page no WebGPU adapter, is told to
   (its `layers.gpu-process.allow-headless` pref). A headless page may also start its sound with no gesture: a browser
   holds an `AudioContext` suspended until the user makes one, and a headless run has no user, so an AudioWorklet
   program rendered nothing until `0.9.943`. Chromium gets `--autoplay-policy=no-user-gesture-required`, and Firefox
   its `media.autoplay.default` and `media.autoplay.block-webaudio` prefs. A run with a window keeps the browser's
   own policy, which is what the program's users get. `--scale 1.5` sets the device scale,
   the page's `devicePixelRatio`, which is 1 in a headless browser: a test of a canvas's pixels sets it, since a
   high-DPI bug hides at 1. `--window 1280x1024` sets the browser window's size in CSS pixels; the page's own size is
   that less whatever frame the browser draws around it. Each is said to the browser its own way (Chromium's
   `--force-device-scale-factor` and `--window-size`; Firefox's `layout.css.devPixelsPerPx` pref and `-width`/`-height`).
5. **It prints the program's output** as the program prints it, each line on the stream it was written to, and
   **exits with the program's status**: `main`'s return or `exit`'s, 134 for an abort (a panic, a trap), and 1 for a
   JavaScript error nothing caught (emscripten's own status for one under node). A page closed, or a browser that
   ends, before the program does ends the run with 129, saying so, with the browser's last lines of output: a Chromium
   downloaded rather than installed cannot start its sandbox on Ubuntu 24.04 and later, which restricts the user
   namespaces it needs, and says so there (Chrome and Edge install the profile that allows it). A browser that has not
   asked for the page within 60 s ends the run with 2, named, with its last lines: one that lost its first navigation
   (an unzipped Chrome for Testing can) would otherwise leave the run waiting in silence. A last line printed without a
   newline arrives with one, since the page hands over whole lines.

`-- <args>` reach the program's `args()`, through the page's URL. Ctrl-C ends the browser and exits 130, and a signal
that ends `kama run` ends the browser with it, saying so if the browser had not yet asked for the page. There is no time
limit on the program, as there is none for a native run: a harness bounds a run. The page reports over plain HTTP, so
any of those browsers runs the program, and no automation protocol is attached unless the program can ask for input
(below).

**Telling the browser more.** Beyond kama's own flags, a run hands the browser what the project or the command line
asks of it. Each setting goes only to the browser family it names, since `$KAMA_BROWSER` or kama's search picks the
browser, not the caller:

| | Chromium, Chrome, Edge | Firefox |
|---|---|---|
| in `kama.json` | `"browser": { "chromiumArgs": [...] }` | `"browser": { "firefoxArgs": [...], "firefoxPrefs": { "<name>": <value> } }` |
| on the command line, repeatable | `--chromium-arg <arg>` | `--firefox-arg <arg>`, `--firefox-pref <name>=<value>` |

The shape is the W3C WebDriver capabilities': `goog:chromeOptions` takes `args`, and `moz:firefoxOptions` takes `args`
and `prefs`. wasm-bindgen's test runner reads the same capabilities from a `webdriver.json`, and Karma's launchers carry
their flags per browser. Emscripten's own `emrun` takes one `--browser_args` for whichever browser runs, which is the
shape this avoids.

- **Order.** The project's settings come first, then a `select.TARGET` entry's `browser`, then the command line's. All of
  them come after kama's own, so where a browser keeps the last of a repeated setting (Chromium's switches, a pref in
  `user.js`), the later one wins.
- **Only the root's.** A dependency's `browser` is read and checked, and never applied: it says how that package's own
  runs start, as Cargo reads `[profile]` from the root manifest alone.
- **A pref's value** is a boolean, an integer or a string, as Firefox keeps it. The type must be the pref's own, since
  Firefox drops a value of another type without a word. `layout.css.devPixelsPerPx` is a string, so it is
  `"2"` in the manifest and `--firefox-pref 'layout.css.devPixelsPerPx="2"'` on the command line. A fraction is
  refused, naming its string spelling. A bare word on the command line is a string.
- **A software GPU.** A page asks WebGPU for its fallback adapter with `requestAdapter({ forceFallbackAdapter: true })`.
  This is what a test that compares rendered pixels byte for byte runs on, a GPU-less CI runner included.
  - **Chromium** offers SwiftShader only when started with `--enable-unsafe-webgpu`: Edge 157 under `kama run`
    answered `null` without it and `google/swiftshader` with it. Chrome for Testing 157 also needs
    `--enable-unsafe-swiftshader` (measured 2026-11-14, Windows 11). An unzipped Chrome for Testing loads a page there
    only with `--disable-features=NetworkServiceSandbox`, which `--chromium-arg` now passes too.
  - **Firefox** answers with WARP as it is started.
  - `--enable-unsafe-webgpu` also lifts Chrome's GPU blocklist, so a page sees WebGPU where that GPU's users may not.
    That is why kama never passes it unasked.

**Input the program asks for.** A UI test drives its own page, and a page cannot make an event the browser takes as the
user's (`isTrusted`), so the program asks its runner: `std::app::runner`'s `input(steps:)`
([SPEC](SPEC.md#application-loop-stdapp-and-web-transports-stdnetweb-)). The runner makes each step over the browser's
automation protocol and answers once the browser has made the last one; the program hears `Done` after the page has
drawn a frame since, so what the input changed is on the page. Every other host answers `NoRunner` at once: a native run,
node, a deployed page, a device.
- **Attached only to a program that can ask.** A program that reaches `std::app::runner` is linked with its glue, and
  its run attaches the protocol: the DevTools protocol over `--remote-debugging-pipe` on Chromium, with no port opened,
  and WebDriver BiDi on Firefox, on a port of `127.0.0.1` it picks and the runner reads from its output. The runner holds
  the browser's one BiDi session. Attaching makes the page's `navigator.webdriver` true, so every other program runs as
  it did, with neither.
- **The browser's own rules hold**, as for a person: a made click lets the page take pointer lock, start an
  `AudioContext` or go fullscreen; a made Tab moves the focus; a drag released outside the canvas ends on the element
  under it; Ctrl+A selects the page, and a drag that starts on the selection becomes the browser's own drag.
- **A key** is its W3C `KeyboardEvent.code` on a US layout (`KeyA`, `Digit1`, `ShiftLeft`, `ArrowUp`, `F1`, `Numpad5`).
  Keys WebDriver has no value for (`CapsLock`, `NumLock`, `PrintScreen`, `ContextMenu`) are refused by name, as is any
  unknown code, before any step of the request is made. A held key's modifier is on every event after it, and a key
  types its character unless Control, Alt or Meta is held. `Text` types what has no key (`é`, an emoji): Chromium takes
  an emoji with a modifier one code point at a time, Firefox whole.
- **What no browser makes**, so no test waits on it: a line wheel (every wheel is in pixels, `deltaMode` 0); a key
  repeat; a change of the device pixel ratio while running (`--scale` sets it at launch); IME composition.
- **Firefox makes no pen**: a request with one is refused with Firefox's reason. Under pointer lock it measures the
  first locked move from the pointer it re-centres, not from the last position, and a made Escape keeps the lock and
  reaches the page as a key, where Chromium gives the lock back and the page sees no Escape.
- **In a window.** A visible run takes requests as a headless one does: measured on Windows, the input took neither the
  desktop's focus nor its keyboard, and reached a page whose window was behind another's. Two things differ. Behind
  another window, Chromium refuses pointer lock and fullscreen. And a pointer lock holds the person's real cursor until
  it ends, on Firefox also from behind another window. A test of either runs headless.

### What a dependency contributes

A dependency's `cflags`, `ldflags` and `link` apply to your build. A package that needs `-lm`, or a
`-D` its own C expects, says so **once, in its own manifest**, and every consumer gets it — you do not
repeat the block, and it cannot drift between your copy and theirs.

The order is fixed: **dependencies first** (alphabetically by import name), **then your project**, then
the command line. So you always have the last word wherever the C compiler resolves a repeated flag
last-wins. `link` is deduped, because it is a list of library *names* and two packages both wanting `m`
is the ordinary case; `cflags`/`ldflags` are not, because they are raw text where a repeat can be
load-bearing.

Each manifest resolves on its own before the lists are joined. So a dependency's
`select.TARGET.<NAME>.link`, which **replaces**, replaces *that package's* list and never yours —
otherwise adding a dependency could delete your `-lm`.

What a dependency **cannot** do is redefine your build:

| it contributes | it does not |
|---|---|
| `cflags`, `cxxflags`, `objcflags`, `ldflags`, `link`, `csources`, `cincludes`, `jsLibraries`, `emSettings`, `embedFiles`, `preloadFiles` | `cc`, `cxx`, `ar`, `sysroot`, `runtime`, `subsystem`, `triple` — your toolchain, your call |
| | `shellFile` — the page your program runs in |
| `reproducible-float` — its own arithmetic and math are what your program runs | `no-heap` — it changes what compiles, program-wide |
| | `webgpu` — it selects an SDK, and could make your build demand a download |
| | `release`, and an `-O` in any flag key — how your program is optimized is your call |
| its own `flags` and `select` groups — names in your build's one flag universe, at its own `default` ([SPEC](SPEC.md#conditional-compilation--compileforflag-)) | setting a flag, its own or another's — your `kama.json`, `kama.local.json` and command line set them; values under `TARGET`, `BUILD_TYPE` or `OUTPUT` |

Two more rules worth knowing. A dependency's **relative** `-I`/`-L` is **refused**, because it would
resolve against *your* working directory rather than against the package — a package's own include tree
is `cincludes`, which kama resolves against the declaring manifest; a machine path stays absolute. A
relative emcc file flag (`--js-library`, `--shell-file`, `--embed-file`, `--preload-file`) in a dependency's
`ldflags` is refused the same way, naming the key that replaces it. And a
dependency whose manifest does not parse stops the build, naming that file: silently skipping it would
silently drop whatever it was contributing. (`kama query` and `kama lsp` stay lenient — an editor sits
above trees you do not own.)

⚠️ Settings are read from the materialized `.kama/deps` view, which is what `kama pkg install` builds
and what the import path already uses. A stale view is a stale build in exactly the same way it is a
stale import; re-run `kama pkg install`.

### 2. You have clang and a sysroot

clang has always been a cross compiler; what it lacks is the target's **headers and libraries**. Point
it at a sysroot and it works:

```sh
kama build app.kama --target aarch64-linux-gnu --cc "clang" \
                    --select OUTPUT=EXE   # plus a sysroot via kama.json, as above
```

kama detects that `clang` is a multi-target driver and passes `-target <triple>` for you. A
**per-target** compiler (`aarch64-linux-gnu-gcc`, a vendor ARM gcc, an NDK wrapper) has its triple in
its name, so kama deliberately does *not* pass `-target` — that would break it. The program's name decides:
`clang`, `clang-18` or a path to `clang.exe` takes `-target`, while a name with a prefix before `clang`
(`aarch64-linux-android29-clang`) is a per-target wrapper.

### 3. You have neither — use zig

[zig](https://ziglang.org/download/) ships the C libraries for essentially every target (musl, glibc
stubs, mingw-w64, wasi-libc), so `zig cc` cross-compiles with no sysroot hunt. One binary, no
per-target installs.

```sh
brew install zig        # or scoop/apt/pacman, or just unpack the tarball
kama build app.kama --target WINDOWS
```

**If `zig` is on your PATH, kama uses it automatically for a cross build** — you do not need `--cc`.
It is only ever chosen when the host compiler genuinely cannot do the job, so ordinary builds are
unaffected. To be explicit, or to use a zig that isn't on PATH:

```sh
kama build app.kama --target WINDOWS --cc "/path/to/zig cc"
```

Kama installs that ship without a system C compiler bundle zig for exactly this reason, and that copy
is used the same way — so on such a machine zig compiles *every* build, not just cross builds.

#### Is `zig cc` as fast as clang?

**Yes — because it *is* clang.** zig bundles LLVM and exposes `zig cc` as a drop-in clang driver, so
the optimizer and code generator are the same ones; `zig cc --version` reports a clang version. kama
passes both compilers identical flags (`-O3 -DNDEBUG -ffunction-sections -fdata-sections` in release)
and **no** `-march`/`-mcpu`, so neither tunes for a specific CPU — both target the architecture's
generic baseline.

Measured on this repo's benchmarks (`bench/src/kama/`), native release builds, best of 5:

| bench | clang | `zig cc` | ratio |
|---|---|---|---|
| collatz | 0.065 s | 0.065 s | **1.00×** |
| pi | 0.012 s | 0.012 s | 1.00× |
| math | 0.007 s | 0.007 s | 0.98× |

(The sub-10 ms rows are dominated by process startup; collatz is the only one long enough to mean
much.) zig's binaries are larger *as files* — but their `__TEXT` is actually **smaller** (1.4 KB vs
16 KB on collatz); the difference is linker metadata and segment padding, not more code.

So there is no performance reason to prefer one, and **your host build does not change** either way:
zig is only substituted when the host compiler genuinely cannot reach the target.

### Always available: emit the C

No toolchain here at all? Transpiling never needs one — it is pure code generation, so it accepts any
target and hands the C to whatever builds it downstream (a vendor IDE, a Yocto recipe, someone else's
CI):

```sh
kama transpile app.kama --target thumbv7em-none-eabihf -o app.c
```

The C is for the target it was transpiled for, not portable C: a compile-time `sizeof`/`alignof` is baked as
that target's number (SPEC *Writing a collection in kama*), and each one carries a `_Static_assert`, so the
same file handed to a compiler for another target stops at the first layout that differs instead of
building a different program.

## What "cross" actually means here

kama only treats a build as cross-compiling when the host compiler genuinely **cannot** do the job:

- a **different architecture**, or
- a **different OS for a hosted target**.

Bare metal on your own architecture is *not* cross — `--target EMBEDDED` is
`-ffreestanding -nostdlib -c` stopping at an object file, which your ordinary clang does perfectly
well. That is what has always made kama's bare-metal builds toolchain-agnostic, and it still holds for
any `os=none` triple on the host arch.

## Output kinds

```sh
kama build app.kama                            # EXE (default)  -> app
kama build app.kama --shared                   # SHARED  -> app.dylib / app.so / app.dll
kama build app.kama --select OUTPUT=STATIC     # STATIC  -> libapp.a
kama build app.kama --select OUTPUT=OBJECT     # OBJECT  -> app.o
kama build kama.json --target aarch64-linux-android29 --select OUTPUT=AAB   # an Android app's App Bundle -> game.aab
```

The extension follows the **target**, so a Windows build from a Mac produces `.dll`, not `.dylib`. A
bare-metal target defaults to `OBJECT` (there is no OS to link against); the board's startup object and
linker script own the final image, which is your link step.

`SHARED` exports only functions marked `expose` (everything else is hidden), so the library's surface
is exactly what you declared.

A library needs no `main`: a C or Objective-C host links `STATIC`, `OBJECT` or `SHARED` output and calls its
`expose`d functions through the header kama writes beside it (`<name>.h`). The runtime's own process-wide state
comes with it. A static library carries it as an archive member of its own, so a kama program that has a `main`
can link the same archive without a duplicate (`tools/check-expose-shared.sh` links both ways).

```sh
kama build game.kama --select OUTPUT=STATIC -o libgame.a     # also writes game.h
cc host.c -I. libgame.a -o host                              # host.c: #include "game.h"
```

## Runtime linkage

A separate axis from the output kind: **how the language's own runtime is linked into whatever you
produce.** The rule is *link non-system runtime statically, system components dynamically* — so a
binary depends only on things the target OS already has.

**Static is the default, and you opt *in* to a DLL:**

```sh
kama build app.kama                        # runtime linked statically
kama build app.kama --dynamic-runtime      # opt in to the runtime DLL
```

or per-project, since linkage is a toolchain property like `cc` and `sysroot`:

```json
"select": { "TARGET": { "WINDOWS": { "runtime": "dynamic" } } }
```

`--dynamic-runtime` wins over the manifest, the same way `--target` beats a declared `"default": true`.

**In practice this only bites on Windows, and on Android for C++ (its libc++ ships with the NDK, not the
device).** On Linux and macOS libc *is* the system — glibc and
libSystem ship with the OS and pthreads live inside libc — so there is no non-system runtime to make a
choice about, and the flag is an accepted no-op there (one build script, no branching). Going further
would be actively wrong: static glibc breaks `dlopen`/NSS, and Apple does not support a static
libSystem at all.

On Windows it matters, because mingw-w64's winpthread ships with your *compiler*, not with Windows.
A program using `isolate` / `parfor` / `channel` used to bind `libwinpthread-1.dll` out of the msys2
tree and die at process start with `STATUS_DLL_NOT_FOUND` (`0xC0000135`) on any machine without it —
including the machine that built it, unless an msys2 shell launched it.

**The C++ runtime follows the same rule**, and it reaches a program through a C++/Objective-C++
`csources` entry. mingw's `libstdc++-6.dll` and `libgcc_s_seh-1.dll` ship with your *compiler*, so a
Windows C++ build used to die the same way (measured `0.9.383`: `tests/csources_cxx.d` exited
`0xC0000135` from a plain PowerShell on the machine that built it). A Windows link that holds C++ now
ends with `-static-libgcc -Wl,-Bstatic`, so the driver's own runtime tail binds statically. ⚠️ Note
what does NOT work, because it is the obvious thing to try: `-static-libstdc++` alone makes libstdc++
static, and mingw's static libstdc++ then needs winpthread — which the C++ driver appends *after* every
argument you pass, so the binary swaps one msys2 DLL for another. The flags are trailing, so libraries
you named are already bound and keep their linkage.

This is the same stance Go, Rust and Zig take: static runtime on Windows, dynamic libc elsewhere.
Libraries **you** name (`--link`, `--webgpu`) are untouched and remain your choice to ship or link —
kama makes the decision only about its own runtime.

Check what you actually produced:

```sh
objdump -p app.exe | grep 'DLL Name'    # want only KERNEL32 + api-ms-win-crt-*
```

## Subsystem

Another Windows-only axis, and the same shape as runtime linkage: **what kind of application the PE
declares itself to be.** Windows gives a *console-subsystem* program a console window whether it wants
one or not — so a GUI program built the default way opens two windows, the console Windows created for
it and then the one it actually asked for.

**Console is the default, and you opt *in* to windows:**

```sh
kama build game.kama --target WINDOWS                        # console subsystem (the default)
kama build game.kama --target WINDOWS --subsystem windows    # no console window
```

…or per-project, since the subsystem is a permanent property of the artifact:

```json
"select": { "TARGET": { "WINDOWS": { "subsystem": "windows" } } }
```

`--subsystem` wins over the manifest, the same way `--dynamic-runtime` does.

**`console` is the default deliberately.** It keeps every console tool, the CI legs and `kama` itself
behaving exactly as they always have, and a GUI app is the thing that knows it is a GUI app. The
alternative default breaks `print` for everyone to fix a stray window for a few.

**`print` still works from a terminal.** The cost that normally comes with `-mwindows` is that
`print`/`eprintln` go nowhere, because a GUI-subsystem process is given no console. kama undoes the half
that matters: at startup a `windows`-subsystem binary calls `AttachConsole(ATTACH_PARENT_PROCESS)` and,
if it was launched *from* a terminal, rebinds its standard descriptors onto that console. So the same
binary is silent when double-clicked from Explorer — correct for a GUI app — and prints normally when run
from PowerShell. Launched from Explorer there is no parent console, the attach fails, and output is
discarded.

**Everywhere else it is an accepted no-op.** No other object format has a subsystem field, so one
cross-platform build script can carry the flag unconditionally — the same stance `--dynamic-runtime`
takes.

Check what you actually produced:

```sh
file app.exe        # PE32+ executable (GUI) … , for MS Windows
```

## Where the artifacts land

Inside a **project** (a directory with a `kama.json`), everything a build generates goes under one root:

```
out/<triple>/<debug|release>/
```

```sh
kama build kama.json                             # out/aarch64-macos-none/debug/myapp
kama build kama.json --release                   # out/aarch64-macos-none/release/myapp
kama build kama.json --target WASM               # out/wasm32-emscripten-none/debug/myapp.html
```

Scoped by **both** axes deliberately. They vary independently, and a collision between them is silent —
you would run yesterday's binary with no diagnostic to tell you so. Cross-compiling for three targets
leaves three trees that never overwrite each other, and `.gitignore` needs one line: `out/`.

The root is the manifest's `"out"` key, defaulting to `out`. An explicit `-o` overrides it entirely.

A **loose `.kama` file with no manifest** is unaffected — `kama build hello.kama` still writes `./hello`
beside the source. `out/` is a project's concept, and one file is not a project.

### `.kama-cache/` — rebuilds skip what did not change

A build keeps each translation unit's object in `.kama-cache/<binary>/` beside the output, and the next build
reuses the ones whose input did not change. It is **on by default**, like Rust's `target/` or Go's build
cache; `--no-cache` turns it off and restores the old behaviour exactly — compile everything, keep nothing.

Measured on a 61-unit project: a cold build is 0.37 s, an unchanged rebuild 0.15 s, and a rebuild after
editing one function body 0.17 s. An edit that changes a DECLARATION (adding a function, changing a
signature) rewrites the shared header every unit includes, so that rebuild is a cold one — the same
behaviour a C project with a shared header has.

What makes an object stale is decided by the compiler, not guessed: each compile passes `-MMD`, and the
cache re-checks the content of every file on the dependency list the compiler wrote, plus the exact command
and the compiler's own `-v` output. So an edited `extern "…"` C header, a changed `cflags`, a kama upgrade
that touches a runtime header and a switch from clang to gcc all rebuild what they must.

Two shapes have no per-unit objects to cache, and get nothing from it: a **`--release`** build (the program
folds into one unity translation unit) and any build pinned to `-j 1`. A **`zig cc`** toolchain brings its
own content-addressed cache, which is why kama clamps it to one invocation and stays out of its way.

## Environment variables

Every variable the toolchain reads, in one place. None is needed for an ordinary build.

| variable | read by | effect |
|---|---|---|
| `KAMA_BUILD_JOBS` | `kama build` | parallel C compiles when `-j` is not given (default: the core count) |
| `KAMA_PATH` | module resolution | extra `:`-separated roots searched for an `import`, after the manifest and before the bundled stdlib. A root is a project or a folder of projects, each found at its full name (`<root>/geo`, `<root>/@acme/geo`); an undeclared root names itself, so a scoped package there is reached only as a declared dependency |
| `KAMA_WGPU_DIR` | `--webgpu`, native | the wgpu-native SDK root, instead of the one `tools/fetch-webgpu.sh` fetched |
| `KAMA_PTHREAD_POOL` | wasm builds | Web Workers to have ready when the page loads, for isolates and the job pool's threads (default 0) |
| `KAMA_BROWSER` | `kama run --target WASM` | the browser the page opens in: a path, or a program on PATH; else Chrome, Edge, Chromium or Firefox ([§ `kama run` in a browser](#kama-run-in-a-browser)) |
| `EM_NODE_JS` / `EMSDK_NODE` | `kama run --target WASM` | the node that serves the page, Emscripten's own; else `node` on PATH |
| `KAMA_LOG` | the built program | the `std::log` filter, e.g. `warn,audio=debug`; `--log` overrides it |
| `ANDROID_NDK_HOME` / `ANDROID_HOME` | Android builds | the NDK, and the SDK whose build-tools and platform package an app, else Android Studio's default ([§ Built-in targets](#built-in-targets)) |
| `JAVA_HOME` | Android apps | the JDK an app is packaged with, else Android Studio's, else `jar` on PATH ([§ Android apps](#android-apps)) |
| `ANDROID_USER_HOME` | Android apps | where the debug key lives (default `~/.android`) |
| `KAMA_KEYSTORE_PASSWORD` / `KAMA_KEY_PASSWORD` | `--release` Android apps | the passwords of `android.keystore` and of its key when that differs; never read from `kama.json` |
| `BUNDLETOOL` | `--select OUTPUT=AAB` | Google's bundletool jar, which builds an App Bundle; else `bundletool` on PATH |
| `KAMA_IO_URING` | the built program, Linux | `0` runs the fiber scheduler on epoll instead of io_uring. Read when an isolate's scheduler is made. The ring is the default wherever the kernel allows it (6.1 or later, and not refused by a container's seccomp, SELinux or `kernel.io_uring_disabled`), and epoll runs everywhere else. Neither changes what a program does. Set it for a server whose clients are mostly on its own host at thousands of connections a core: loopback charges the server for its client's receive of every reply, which costs more under the ring, so epoll serves 14% more at 5,000 a core and 2.4% fewer at 500 ([benchmarks/servers.md](benchmarks/servers.md#at-10000-connections-on-loopback-the-clients-receive-charged-to-the-server)). A process on the ring holds an `anon_inode:[io_uring]` descriptor |
| `KAMA_VERSION` | the `kama` selector, `install.sh` | run (or install) this toolchain version, `vX.Y.Z` ([packages.md](packages.md#toolchain-versions)) |
| `KAMA_NO_SELECT` | the `kama` selector | run the binary you invoked, ignoring every pin |
| `KAMA_STORE` | `kama pkg` | the shared package store (default `~/.kama/store`) |
| `KAMA_HOME` / `KAMA_NO_STD` | `install.sh` | install prefix (default `~/.kama`); skip the bundled stdlib |
| `KAMA_LSP_MAX_FILES` | `kama lsp` | the cap on a guessed, manifest-less project ([editors.md](editors.md)) |
| `KAMA_TIMING` | every analysis | `1` prints one `kama-timing:` line per analysis to stderr; `2` adds one per unit |
| `KAMA_PRUNE_TRACE` / `KAMA_NO_PRUNE` | module resolution | report what import pruning decided / turn pruning off — diagnosis only |

## Platform notes

- **macOS → anything**: fine with zig or a sysroot.
- **anything → macOS**: needs the macOS SDK, which Apple's licence keeps on Apple hardware. Practical
  answer: build macOS artifacts on a Mac (or a Mac CI runner).
- **→ Windows**: the runtime links statically, so the `.exe` is distributable as-is — see
  [Runtime linkage](#runtime-linkage). `WINDOWS` targets the **mingw-w64** ABI (`-gnu`), which zig covers. An MSVC-ABI build
  (`x86_64-windows-msvc`) needs the MSVC headers and libraries, so build it on Windows.
- **→ wasm**: uses Emscripten (`emcc`), not zig — it emits an `.html` + `.js` + `.wasm` harness, and
  `$EMCC` or `--cc` overrides which `emcc`. `kama run` runs the page in a browser
  ([§ `kama run` in a browser](#kama-run-in-a-browser)). Builds pass **`-msimd128`** (both debug and release, so the
  two tiers share one instruction set), which is what makes `std::math` vectorize here as it does on
  native — without it a `.wasm` holds no vector instruction at all. That sets the **runtime baseline**:
  wasm SIMD needs **node ≥ 16** or any current browser. It is the one place kama does not target the
  bare generic baseline, and it is deliberate: every engine that can run a 2026 `.wasm` has it. A wasm module starts
  no process, so a wasm target does not derive `CHILD_PROCESSES` and `std::process`'s `Command` is refused by name, and its
  file system reports no change, so it does not derive `FILE_WATCH` and `std::fs`'s `Watcher` is refused by name;
  its user database has no records (uid 0, `UserId.homeDirectory()` is `NotFound`), and `$HOME` is `/home/web_user`.
- **→ bare metal**: for the host arch, nothing extra. For a real board, name its triple and give it a
  `cc` (`arm-none-eabi-gcc`, or `zig cc`). See [mcu.md](mcu.md) for the full firmware flow. Name the core in `cflags` (`-mcpu=cortex-m0`,
  `-march=rv32imc`), not only the triple: the C compiler's view of the chip decides which `Atomic` widths are native and
  which mask interrupts ([mcu.md](mcu.md#atomic-on-a-chip-without-the-instruction)).

## Troubleshooting

**"cannot build for X — the default C compiler has no libc for it"**
kama will not guess a toolchain that cannot work. Pick a route from *Getting a cross toolchain* above,
or `kama transpile` and build the C elsewhere.

**On Windows the link fails with `ld: cannot find …\???\<name>-xxxxxx.o`, and every path I passed is ASCII.**
Your `%TEMP%` contains non-ASCII characters — which is what a Windows account name like `Björn` or
`日本語` gives you. It is the toolchain, not kama: on a single-invocation build clang compiles to a
temporary object under `%TEMP%` and hands *that* to GNU `ld`, which is a narrow program and cannot open
a non-ASCII path in either direction. Nothing on the command line has to be non-ASCII for this to fire.
Three ways out, any one of which is enough: add `-fuse-ld=lld` (LLVM's linker reads UTF-16 and has
neither limit), point `TEMP`/`TMP` at an ASCII directory, or let kama's per-translation-unit path do
the link — `-j 2` or higher never asks clang for a temporary, so only a one-TU build (or `-j 1`) is
exposed. The same narrowness in `ld`/`ar` is why kama hands them 8.3 aliases for its own paths; see
[platforms/windows.md](platforms/windows.md) for the measurements.

**My Windows `.exe` dies immediately with `0xC0000135` / "the code execution cannot proceed".**
That is `STATUS_DLL_NOT_FOUND` at process start — a DLL the binary imports is not on the machine.
`objdump -p app.exe | grep 'DLL Name'` names it. If it is `libwinpthread-1.dll`, you built with
`--dynamic-runtime` or a `"runtime": "dynamic"` target; drop it and the runtime links in statically.
If it is a library **you** named (`--link`, `--webgpu`), ship that DLL beside the `.exe` — kama does
not decide linkage for your dependencies. See [Runtime linkage](#runtime-linkage).

**I want to tune for a specific CPU.**
kama passes no `-march`/`-mcpu`/`-mtune`, so builds target the architecture's generic baseline — which
is what makes them portable. To override, put it in the target's `cflags`:
`"RPI": { "triple": "aarch64-linux-gnu", "cflags": ["-mcpu=cortex-a72"] }`. There is deliberately no
`--march=native` shorthand yet; it is a recorded follow-on (ROADMAP_DETAIL §10).

**A `@compileFor` gate isn't firing.**
Gate on the **derived** facts, not on how you spelled the build. `--target EMBEDDED` and
`--target xtensa-none-elf` both give you `OS_NONE`; neither gives you a flag called `EMBEDDED`, because
a built-in target name describes the invocation rather than the target. The flags a target contributes
are `ARCH_<arch>`, `OS_<os>`, `ABI_<abi>`, `HOSTED` when the target has an OS, `SIMD128` when its baseline has
128-bit vectors, `CHILD_PROCESSES` when its programs may start one (Linux, Android, macOS, Windows, FreeBSD), and
`FILE_WATCH` when std has a file watcher for it (Linux, Android, Windows).

**Two values of one group.**
`--select CONSOLE=PS5 --select CONSOLE=XBOX` is an error: a `select` group is single-select by
definition. Use `--define` / `flags` for things that combine.
