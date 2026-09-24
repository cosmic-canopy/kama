# kama installer:  irm https://kama-lang.org/install.ps1 | iex
#
# Detects arch and whether a C compiler is present, then installs the matching
# release into %USERPROFILE%\.kama\versions\<version>\. The `kama` on PATH is a thin selector.
#   -NoStd / $env:KAMA_NO_STD=1   skip the bundled standard library
#   $env:KAMA_VERSION=vX.Y.Z      install a specific release (default: latest)
#   $env:KAMA_SET_DEFAULT=1       make this version the global default (else set only on first install)
#   $env:KAMA_HOME=<dir>          install prefix (default: %USERPROFILE%\.kama)
param([switch]$NoStd)
$ErrorActionPreference = 'Stop'
$Repo   = 'cosmic-canopy/kama'
$Prefix = if ($env:KAMA_HOME) { $env:KAMA_HOME } else { "$env:USERPROFILE\.kama" }
$Ver    = if ($env:KAMA_VERSION) { $env:KAMA_VERSION } else { 'latest' }
$arch   = if ([Environment]::Is64BitOperatingSystem) { 'x64' } else { throw 'kama-install: unsupported arch' }

$hasCC  = @('cl','clang','gcc') | Where-Object { Get-Command $_ -ErrorAction SilentlyContinue }
$flavor = if ($hasCC) { '' } else { '-bundled' }
Write-Host "kama-install: $(if ($hasCC) {'C compiler found - slim build'} else {'no compiler - self-contained build (bundled zig cc)'})"

if ($Ver -eq 'latest') { $Ver = (Invoke-RestMethod "https://api.github.com/repos/$Repo/releases/latest").tag_name }
$asset = "kama-windows-$arch$flavor-$Ver.tar.gz"
$url   = "https://github.com/$Repo/releases/download/$Ver/$asset"

$tmp = New-Item -ItemType Directory -Path ([IO.Path]::GetTempPath()) -Name ([Guid]::NewGuid())
Write-Host "kama-install: downloading $asset"
Invoke-RestMethod $url -OutFile "$tmp\$asset"
try {
  $want = (Invoke-RestMethod "$url.sha256").Split(' ')[0]
  $got  = (Get-FileHash "$tmp\$asset" -Algorithm SHA256).Hash
  if ($want -and $got -ne $want) { throw 'checksum mismatch' }
  Write-Host 'kama-install: checksum ok'
} catch { Write-Host 'kama-install: warning: checksum not verified' }

# Versioned store: each toolchain in its own dir; the shared package store ($Prefix\store) is untouched. A
# versioned install never overwrites a running .exe (a new version is a new dir) — no move-aside needed.
$vdir = "$Prefix\versions\$Ver"
New-Item -ItemType Directory -Force -Path $vdir | Out-Null
tar -xzf "$tmp\$asset" -C $vdir --strip-components=1          # tar ships with Windows 10+
if ($NoStd -or $env:KAMA_NO_STD) { Remove-Item -Recurse -Force "$vdir\lib\kama" -EA SilentlyContinue; Write-Host 'kama-install: skipped stdlib' }
Write-Host "kama-install: installed kama $Ver to $vdir"

# The PATH selector + global default: on explicit request ($env:KAMA_SET_DEFAULT) or the first-ever install.
$bin = "$Prefix\bin"
if ($env:KAMA_SET_DEFAULT -eq '1' -or -not (Test-Path "$Prefix\default")) {
  New-Item -ItemType Directory -Force -Path $bin | Out-Null
  # Windows can't overwrite a running .exe — move the old selector aside, then copy the new one in.
  if (Test-Path "$bin\kama.exe") { Move-Item "$bin\kama.exe" "$bin\kama.exe.old" -Force -EA SilentlyContinue }
  Copy-Item "$vdir\bin\kama.exe" "$bin\kama.exe" -Force
  # ...and the runtime headers + stdlib BESIDE it. The compiler resolves both relative to its own
  # executable (`<exeDir>\..\include`, `<exeDir>\..\lib\kama`), so a COPY at $Prefix\bin looks for
  # $Prefix\include and $Prefix\lib — which did not exist. `kama check` worked and `kama build` died in
  # the C compiler with "'kama_runtime.h' file not found". POSIX fixes this with a symlink into the
  # versioned toolchain; Windows cannot, because a file symlink needs Developer Mode or admin, so the
  # default toolchain is laid out flat next to bin\ instead.
  Copy-Item "$vdir\include" "$Prefix\include" -Recurse -Force
  if (Test-Path "$vdir\lib") { Copy-Item "$vdir\lib" "$Prefix\lib" -Recurse -Force }
  Remove-Item "$bin\kama.exe.old" -Force -EA SilentlyContinue
  Set-Content -Path "$Prefix\default" -Value $Ver -NoNewline
  Write-Host "kama-install: default is now kama $Ver"
}

if (-not ($env:Path -split ';' | Where-Object { $_ -eq $bin })) {
  [Environment]::SetEnvironmentVariable('Path', "$bin;$([Environment]::GetEnvironmentVariable('Path','User'))", 'User')
  Write-Host 'kama-install: added to your user PATH (restart the shell)'
}
& "$bin\kama.exe" --version
