# KR-378 decision 10 (docs/ROADMAP_DETAIL.md `s10-trusted-input`): does input made over the browser's protocol take the
# desktop's focus or keyboard when the browser's window is visible? Windows only (Windows PowerShell 5.1).
#
# A small "kr378 focus holder" window takes the focus first. A key that reaches the desktop rather than the page lands in
# its text box. Each browser runs twice:
#   A  the browser keeps whatever focus Windows gives it at launch;
#   B  the holder takes the focus back before step 1 (probe.mjs --hold gives it the time).
# The foreground window, the cursor's clip rectangle and the session's last-input tick are sampled every 20 ms and
# logged when they change. Each run writes <browser>-<A|B>.json (read it with summarize.mjs) and .stderr.txt; the log is
# focus.log. The page's own document.hasFocus() is in the JSON, per event.
#
# Usage: powershell -NoProfile -STA -ExecutionPolicy Bypass -File visible.ps1 -Node <node.exe> -OutDir <dir> <browser.exe>...
param([Parameter(Mandatory)][string]$Node, [Parameter(Mandatory)][string]$OutDir,
      [Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Browsers)
$ErrorActionPreference = 'Stop'
$probe = Join-Path $PSScriptRoot 'probe.mjs'
New-Item -ItemType Directory -Force $OutDir | Out-Null
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices; using System.Text;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [StructLayout(LayoutKind.Sequential)] struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] static extern bool GetClipCursor(out RECT r);
  [StructLayout(LayoutKind.Sequential)] struct LII { public uint cbSize; public uint dwTime; }
  [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LII i);
  public static uint LastInput() { var i = new LII(); i.cbSize = 8; GetLastInputInfo(ref i); return i.dwTime; }
  public static string Title(IntPtr h) { var s = new StringBuilder(256); GetWindowText(h, s, 256); return s.ToString(); }
  public static string Clip() { RECT r; GetClipCursor(out r); return r.L + "," + r.T + "," + r.R + "," + r.B; }
}
'@

$clock = [Diagnostics.Stopwatch]::StartNew()
$log = New-Object Collections.Generic.List[string]
function Note([string]$s) { $line = '{0,6} ms  {1}' -f $clock.ElapsedMilliseconds, $s; $log.Add($line); Write-Host $line }

$form = New-Object Windows.Forms.Form
$form.Text = 'kr378 focus holder'; $form.Width = 380; $form.Height = 110; $form.StartPosition = 'Manual'
$wa = [Windows.Forms.Screen]::PrimaryScreen.WorkingArea
$form.Left = $wa.Right - 420; $form.Top = $wa.Top + 40
$box = New-Object Windows.Forms.TextBox; $box.Left = 10; $box.Top = 10; $box.Width = 340; $form.Controls.Add($box)
$form.Show(); [Windows.Forms.Application]::DoEvents()

function Describe([IntPtr]$h) {
  if ($h -eq $form.Handle) { return 'HOLDER' }
  if ($h -eq [IntPtr]::Zero) { return '(none)' }
  $p = [uint32]0; [void][W]::GetWindowThreadProcessId($h, [ref]$p)
  $n = try { (Get-Process -Id $p -ErrorAction Stop).ProcessName } catch { '?' }
  "$n pid $p '$([W]::Title($h))'"
}
# The foreground lock lets a process raise its own window only in some states; joining the foreground thread's input
# first is the documented way round it. No input is made.
function Take-Focus {
  $p = [uint32]0; $t = [W]::GetWindowThreadProcessId([W]::GetForegroundWindow(), [ref]$p); $me = [W]::GetCurrentThreadId()
  [void][W]::AttachThreadInput($me, $t, $true)
  [void][W]::SetForegroundWindow($form.Handle); [void][W]::BringWindowToTop($form.Handle)
  [void][W]::AttachThreadInput($me, $t, $false)
  $form.Activate(); [void]$box.Focus(); [Windows.Forms.Application]::DoEvents()
}
function Read-Shared([string]$f) {
  try { $s = [IO.File]::Open($f, 'Open', 'Read', 'ReadWrite'); $r = New-Object IO.StreamReader($s); $t = $r.ReadToEnd(); $r.Close(); $t }
  catch { '' }
}

Note "cursor clip at rest: $([W]::Clip())"
foreach ($b in $Browsers) {
  foreach ($mode in 'A', 'B') {
    $tag = '{0}-{1}' -f [IO.Path]::GetFileNameWithoutExtension($b), $mode
    $json = Join-Path $OutDir "$tag.json"; $err = Join-Path $OutDir "$tag.stderr.txt"
    Take-Focus; $box.Text = ''
    Note "== $tag; foreground before launch: $(Describe ([W]::GetForegroundWindow()))"
    $argLine = '"{0}" "{1}" --hold=1500 --out="{2}"' -f $probe, $b, $json
    $p = Start-Process -FilePath $Node -ArgumentList $argLine -NoNewWindow -PassThru -RedirectStandardError $err
    $null = $p.Handle   # keeps ExitCode readable after the process ends
    $lastH = [IntPtr]::Zero; $lastTitle = $null; $lastClip = $null; $lastIn = [W]::LastInput(); $took = $false
    while (-not $p.HasExited) {
      [Windows.Forms.Application]::DoEvents()
      $h = [W]::GetForegroundWindow(); $title = [W]::Title($h)
      if ($h -ne $lastH -or $title -ne $lastTitle) { Note "foreground: $(Describe $h)"; $lastH = $h; $lastTitle = $title }
      $clip = [W]::Clip(); if ($clip -ne $lastClip) { Note "cursor clip: $clip"; $lastClip = $clip }
      $in = [W]::LastInput(); if ($in -ne $lastIn) { Note "last-input tick moved +$($in - $lastIn) ms"; $lastIn = $in }
      if ($mode -eq 'B' -and -not $took -and (Read-Shared $err) -match 'holding') {
        Take-Focus; $took = $true; Note "holder took the focus back: $(Describe ([W]::GetForegroundWindow()))"
      }
      Start-Sleep -Milliseconds 20
    }
    Note "probe exited $($p.ExitCode); holder's text box: '$($box.Text)'"
    (Read-Shared $err) -split "`n" | Where-Object { $_ } | ForEach-Object { $log.Add("        probe $_") }
  }
}
$form.Close()
$log | Set-Content -Encoding utf8 (Join-Path $OutDir 'focus.log')
