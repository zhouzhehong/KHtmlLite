# tests/run_regression.ps1 — sequential probe regression for KHtmlLite.
#
# Runs the GUI/offline probes ONE AT A TIME (they automate real windows, so they
# must never overlap), records each probe's tail output and exit code, writes a
# report, and exits non-zero if any probe failed.
#
# A probe counts as failed when its exit code is non-zero OR its output carries
# no PASS/INCONCLUSIVE verdict at all (a probe that silently did nothing must
# not be recorded as a pass).  INCONCLUSIVE (the real-site bing check when the
# detector finds no blue element to measure) is recorded as a warning, not a
# failure — see the summary line.
#
# Usage:
#   pwsh -File tests/run_regression.ps1
#   pwsh -File tests/run_regression.ps1 -Skip bing_ghost_check.py
#   pwsh -File tests/run_regression.ps1 -Skip detector-selftest,bing_ghost_check.py
#
# bing_ghost_check.py needs the network and a live browser window; skip it when
# running offline.  Its --selftest (registered as "detector-selftest", run
# first, fully offline) pins the ghost detector's thresholds, so a broken
# detector is still caught without network.
# Every probe owns and kills only the processes it started.

param(
    [string[]]$Skip = @(),
    [string]$Out = "tests\regression_report.txt"
)

# `pwsh -File script.ps1 -Skip a,b` passes "a,b" as ONE string (comma lists are
# not split for -File), so accept both forms: array and comma-separated string.
$Skip = @($Skip | ForEach-Object { $_ -split "," } | Where-Object { $_ })

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

# System python (has PIL); never the msys64 one.
$py = (Get-Command python -All |
        Where-Object { $_.Source -notlike "*msys64*" } |
        Select-Object -First 1).Source
if (-not $py) { Write-Host "no python found"; exit 2 }

# Remove any earlier report first: a stale file would satisfy the freshness
# check below even when this run fails to write its own.
if (Test-Path $Out) { Remove-Item -LiteralPath $Out -Force }

$lines = @()
$lines += "KHtmlLite probe regression"
$lines += "python : $py"
$lines += "started: $(Get-Date -Format o)"
$lines += ""
$failed = @()
$inconclusive = @()

function Invoke-Probe {
    param([string]$Name, [string[]]$Extra = @(), [string]$Label = "",
          [bool]$MatchName = $true)

    $shown = ($Name + " " + ($Extra -join " ")).Trim()
    $id = if ($Label) { $Label } else { $Name }
    # -MatchName $false lets a run share a file name with another but be skipped
    # only by its own -Label (the offline selftest must stay runnable when the
    # network probe is skipped).
    if (($MatchName -and ($Skip -contains $Name)) -or ($Skip -contains $id)) {
        $script:lines += "=== SKIP $id ($shown)"
        Write-Host "=== SKIP $id"
        return
    }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "=== RUN $shown"
    $out = & $py -u "tests\$Name" @Extra 2>&1
    $code = $LASTEXITCODE
    $sw.Stop()
    $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    $text = ($out | Out-String)
    $tail = @($out | Select-Object -Last 25)

    # Verdict: exit code alone is not enough (a probe can print FAIL and still
    # return 0), so require a verdict token as well.
    $verdict = ""
    if ($text -match "INCONCLUSIVE") { $verdict = "INCONCLUSIVE" }
    if ($text -match "\bPASS\b") { $verdict = if ($verdict -eq "INCONCLUSIVE") { "PASS+INCONCLUSIVE" } else { "PASS" } }

    $script:lines += "=== $shown  exit=$code  seconds=$secs  verdict=$($verdict -replace '^$','NONE')"
    $script:lines += $tail
    $script:lines += ""
    Write-Host ($tail -join "`n")
    Write-Host "--- exit=$code seconds=$secs verdict=$($verdict -replace '^$','NONE')"

    if ($code -ne 0) {
        $script:failed += "$shown (exit $code)"
    } elseif (-not $verdict) {
        $script:failed += "$shown (no PASS/INCONCLUSIVE verdict)"
    } elseif ($verdict -match "INCONCLUSIVE") {
        $script:inconclusive += $shown
    }
}

# Offline first: the ghost detector must find a synthetic ghost and reject a
# clean frame, so a detector that silently stopped detecting is caught even
# when the network/probe artifacts are unavailable.
Invoke-Probe "bing_ghost_check.py" -Extra @("--selftest") -Label "detector-selftest" -MatchName $false

# The 10 standing probes, plus the two diagnostics that carry the ghost-pixel
# fix evidence (rg_diag4 = no-resize control, bing = real site).
$probes = @(
    "bg_smoke.py",
    "browser_click_probe.py",
    "dpr_probe.py",
    "input_coord_probe.py",
    "resize_probe.py",
    "scroll_smoke.py",
    "tile_protocol_test.py",
    "title_probe.py",
    "tombstone_probe.py",
    "resize_ghost_probe.py",
    "rg_diag4.py",
    "bing_ghost_check.py"
)
foreach ($p in $probes) { Invoke-Probe $p }

$lines += "finished: $(Get-Date -Format o)"
$lines += "failed: " + $(if ($failed.Count) { $failed -join ", " } else { "none" })
$lines += "inconclusive: " + $(if ($inconclusive.Count) { $inconclusive -join ", " } else { "none" })
# No -Encoding: some sandboxed PowerShell hosts reject that parameter, and the
# failure is non-terminating — so verify the file landed instead of trusting it.
$lines | Set-Content -Path $Out
if (-not (Test-Path $Out)) {
    Write-Host "could not write report to $Out"
    exit 2
}

Write-Host ""
Write-Host "REGRESSION SUMMARY: failed=$($failed.Count) [$($failed -join ', ')]  inconclusive=$($inconclusive.Count) [$($inconclusive -join ', ')]  report=$Out"
if ($failed.Count -gt 0) { exit 1 }
exit 0
