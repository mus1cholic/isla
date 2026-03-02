Write-Host "Checking clang-format policy..."
$workspace = [System.IO.Path]::GetFullPath($env:GITHUB_WORKSPACE)
$reportPath = Join-Path $workspace "format-check-warnings.md"
if (Test-Path $reportPath) {
  Remove-Item $reportPath -Force
}
$newlineCountRaw = $env:NEWLINE_COUNT
$newlineCount = if ($newlineCountRaw) { [int]$newlineCountRaw } else { 0 }
$newlineReportPath = $env:NEWLINE_REPORT_PATH
$combinedViolations = New-Object System.Collections.Generic.List[string]
function Add-CollapsibleViolation([string]$summary, [string]$details) {
  $combinedViolations.Add("<details>")
  $combinedViolations.Add("<summary>$summary</summary>")
  $combinedViolations.Add("")
  $combinedViolations.Add($details)
  $combinedViolations.Add("")
  $combinedViolations.Add("</details>")
  $combinedViolations.Add("")
}
$totalViolationCount = $newlineCount
if ($newlineCount -gt 0 -and $newlineReportPath -and (Test-Path $newlineReportPath)) {
  foreach ($line in Get-Content -Path $newlineReportPath) {
    if ($line -like "- *") {
      if ($line -match '^- `(?<file>.+)` \((?<reason>.+)\)$') {
        $summary = "<code>$($Matches.file)</code> (newline policy)"
        $details = "$($Matches.reason)"
      } else {
        $summary = "newline policy violation"
        $details = $line
      }
      Add-CollapsibleViolation $summary $details
    }
  }
}

$changedLinesByFile = @{}
if ($env:DIFF_BASE -and $env:DIFF_HEAD) {
  $currentFile = $null
  $diffLines = & git diff --unified=0 --no-color --no-prefix --diff-filter=ACMR $env:DIFF_BASE $env:DIFF_HEAD -- client engine shared
  foreach ($diffLine in $diffLines) {
    if ($diffLine -like "+++ *") {
      $path = $diffLine.Substring(4).Trim()
      if ($path -eq "/dev/null") {
        $currentFile = $null
      } else {
        $currentFile = $path -replace '\\', '/'
        if (-not $changedLinesByFile.ContainsKey($currentFile)) {
          $changedLinesByFile[$currentFile] = [System.Collections.Generic.HashSet[int]]::new()
        }
      }
      continue
    }

    if (-not $currentFile) {
      continue
    }

    if ($diffLine -match '^@@ -\d+(?:,\d+)? \+(?<start>\d+)(?:,(?<count>\d+))? @@') {
      $start = [int]$Matches.start
      $count = if ($Matches.count) { [int]$Matches.count } else { 1 }
      for ($i = 0; $i -lt $count; $i++) {
        $null = $changedLinesByFile[$currentFile].Add($start + $i)
      }
    }
  }
}

$files = @(Get-Content -Path $env:CHANGED_FILES_FILE | Where-Object {
  $_ -and
  ($_ -like "client/*" -or $_ -like "engine/*" -or $_ -like "shared/*") -and
  ($_ -like "*.cpp" -or $_ -like "*.hpp" -or $_ -like "*.h") -and
  ($_ -notlike "third_party/*")
})
Write-Host ("clang-format candidate files: {0}" -f $files.Count)
if (-not $files) {
  Write-Host "No C++ source files found."
  if ($combinedViolations.Count -gt 0) {
    "Formatting changes required in the following files:" | Out-File -FilePath $reportPath -Encoding utf8
    $combinedViolations | Out-File -FilePath $reportPath -Append -Encoding utf8
  } else {
    "No format violations." | Out-File -FilePath $reportPath -Encoding utf8
  }
  "violations_count=$totalViolationCount" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  "report_path=$reportPath" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  exit 0
}

$formatViolationsObj = New-Object System.Collections.Concurrent.ConcurrentBag[object]

$files | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
  $file = $_
  $cbByFile = $using:changedLinesByFile
  $tempDir = $using:env:RUNNER_TEMP

  if (-not (Test-Path -LiteralPath $file)) {
    return
  }

  $formatOutput = & clang-format --dry-run --Werror --ferror-limit=0 --style=file $file 2>&1
  $exitCode = $LASTEXITCODE
  $hadViolation = ($exitCode -ne 0) -or ($formatOutput -match "code should be clang-formatted")

  if ($hadViolation) {
    $lineNumbers = @()
    $outStrings = if ($null -ne $formatOutput) { $formatOutput | ForEach-Object { $_.ToString() } } else { @() }

    foreach ($outLine in $outStrings) {
      if ($outLine -match '^(?<path>.+?):(?<line>\d+):(?<col>\d+):\s+error:\s+code should be clang-formatted') {
        $line = [int]$Matches.line
        if ($cbByFile.ContainsKey($file)) {
          if ($cbByFile[$file].Contains($line)) {
            $lineNumbers += $line
          }
        } else {
          $lineNumbers += $line
        }
      }
    }

    $uniqueLines = @($lineNumbers | Sort-Object -Unique)
    if ($uniqueLines.Count -eq 0) {
      $lineSummary = "unknown"
    } else {
      $lineSummary = ($uniqueLines | ForEach-Object { $_.ToString() }) -join ", "
    }

    $diffSnippet = "_diff unavailable_"
    $tmpFormatted = Join-Path $tempDir ("clang-format-" + [Guid]::NewGuid().ToString() + ".tmp")
    try {
      & clang-format --style=file $file | Out-File -FilePath $tmpFormatted -Encoding utf8NoBOM
      $rawDiff = & git --no-pager diff --no-index --unified=2 -- $file $tmpFormatted 2>&1
      if ($LASTEXITCODE -eq 1) {
        $diffBody = @($rawDiff | Where-Object {
          $_ -notmatch '^(diff --git |index |--- |\+\+\+ )'
        })
        if ($diffBody.Count -gt 60) {
          $diffBody = $diffBody[0..59] + @("... (truncated)")
        }
        $joinedDiff = $diffBody -join "`n"
        $diffSnippet = '```diff' + "`n" + $joinedDiff + "`n" + '```'
      }
    } finally {
      if (Test-Path $tmpFormatted) {
        Remove-Item $tmpFormatted -Force
      }
    }

    $violation = [PSCustomObject]@{
      File    = $file
      Lines   = @($lineSummary)
      Snippet = $diffSnippet
      Output  = $outStrings
    }
    $concurrentBag = $using:formatViolationsObj
    $concurrentBag.Add($violation)
  }
}

$formatViolations = $formatViolationsObj | Sort-Object File

foreach ($violation in $formatViolations) {
  $totalViolationCount += 1
  Write-Host ('::error file=' + $violation.File + '::clang-format check failed')
  if ($violation.Output) {
    $violation.Output | ForEach-Object { Write-Host $_ }
  }
}

if ($formatViolations.Count -gt 0) {
  Write-Host "Files violating clang-format rules:"
  foreach ($violation in $formatViolations) {
    $lineSummary = ($violation.Lines | Select-Object -Unique) -join ", "
    Write-Host ("{0}: line(s) {1}" -f $violation.File, $lineSummary)
    $summary = ("<code>{0}</code> (clang-format lines: {1})" -f $violation.File, $lineSummary)
    $details = $violation.Snippet
    Add-CollapsibleViolation $summary $details
  }

  "Formatting changes required in the following files:" | Out-File -FilePath $reportPath -Encoding utf8
  $combinedViolations | Out-File -FilePath $reportPath -Append -Encoding utf8
  "violations_count=$totalViolationCount" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  "report_path=$reportPath" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  exit 1
}

if ($combinedViolations.Count -gt 0) {
  "Formatting changes required in the following files:" | Out-File -FilePath $reportPath -Encoding utf8
  $combinedViolations | Out-File -FilePath $reportPath -Append -Encoding utf8
} else {
  "No format violations." | Out-File -FilePath $reportPath -Encoding utf8
}
"violations_count=$totalViolationCount" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
"report_path=$reportPath" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
