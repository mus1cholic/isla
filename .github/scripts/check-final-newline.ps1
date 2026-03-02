Write-Host "Checking final newline policy..."
$workspace = [System.IO.Path]::GetFullPath($env:GITHUB_WORKSPACE)
$newlineReportPath = Join-Path $workspace "newline-check-warnings.md"
if (Test-Path $newlineReportPath) {
  Remove-Item $newlineReportPath -Force
}
$newlineViolations = @()
$allTrackedFiles = @(Get-Content -Path $env:CHANGED_FILES_FILE | Where-Object {
  $_ -and $_ -notlike "third_party/*"
})
Write-Host ("Scanned file candidates: {0}" -f $allTrackedFiles.Count)
try {
  foreach ($file in $allTrackedFiles) {
    if (-not (Test-Path -LiteralPath $file)) {
      continue
    }

    $stream = [System.IO.File]::OpenRead($file)
    try {
      if ($stream.Length -eq 0) {
        continue
      }

      $probeLength = [Math]::Min(4096, [int]$stream.Length)
      $probe = New-Object byte[] $probeLength
      $read = $stream.Read($probe, 0, $probeLength)
      $isBinary = $false
      for ($i = 0; $i -lt $read; $i++) {
        if ($probe[$i] -eq 0) {
          $isBinary = $true
          break
        }
      }
      if ($isBinary) {
        continue
      }

      # Enforce exactly one trailing newline in constant memory:
      # - final byte must be LF
      # - byte sequence before the final line ending must not itself end with LF
      $null = $stream.Seek(-1, [System.IO.SeekOrigin]::End)
      $lastByte = $stream.ReadByte()
      if ($lastByte -ne 10) {
        $newlineViolations += [PSCustomObject]@{ File = $file; Reason = "Missing final newline" }
        continue
      }

      if ($stream.Length -ge 2) {
        $null = $stream.Seek(-2, [System.IO.SeekOrigin]::End)
        $prevByte = $stream.ReadByte()
        if ($prevByte -eq 10) {
          $newlineViolations += [PSCustomObject]@{ File = $file; Reason = "Multiple trailing newlines" }
          continue
        }
        if ($prevByte -eq 13 -and $stream.Length -ge 3) {
          $null = $stream.Seek(-3, [System.IO.SeekOrigin]::End)
          $prevPrevByte = $stream.ReadByte()
          if ($prevPrevByte -eq 10) {
            $newlineViolations += [PSCustomObject]@{ File = $file; Reason = "Multiple trailing newlines" }
          }
        }
      }
    } finally {
      $stream.Dispose()
    }
  }
} catch {
  Write-Host "::error::Final newline check crashed: $($_.Exception.Message)"
  throw
}

if ($newlineViolations.Count -gt 0) {
  Write-Host "Files violating final newline policy (exactly one trailing newline required):"
  "Final newline policy violations:" | Out-File -FilePath $newlineReportPath -Encoding utf8
  foreach ($violation in $newlineViolations) {
    Write-Host "$($violation.File): $($violation.Reason)"
    Write-Host ('::error file=' + $violation.File + '::' + $violation.Reason)
    ('- `{0}` ({1})' -f $violation.File, $violation.Reason) | Out-File -FilePath $newlineReportPath -Append -Encoding utf8
  }
  "newline_count=$($newlineViolations.Count)" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  "newline_report_path=$newlineReportPath" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  exit 1
}

"No newline violations." | Out-File -FilePath $newlineReportPath -Encoding utf8
"newline_count=0" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
"newline_report_path=$newlineReportPath" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
