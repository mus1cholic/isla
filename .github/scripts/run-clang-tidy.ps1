$workspace = [System.IO.Path]::GetFullPath($env:GITHUB_WORKSPACE)
$logPath = Join-Path $workspace "clang-tidy.log"
$reportPath = Join-Path $workspace "clang-tidy-warnings.md"
$bazelFlags = @(& "$workspace/.github/scripts/get-bazel-remote-flags.ps1")
$compileCommandsPath = Join-Path $workspace "compile_commands.json"
if (-not (Test-Path $compileCommandsPath)) {
  throw "Missing compile_commands.json at $compileCommandsPath. Ensure //tools:refresh_compile_commands_editor ran successfully."
}

if (Test-Path $logPath) { Remove-Item $logPath -Force }
if (Test-Path $reportPath) { Remove-Item $reportPath -Force }
# Always create output files up front so artifact upload and PR reporting
# have a stable file path even when clang-tidy emits no output.
New-Item -ItemType File -Path $logPath -Force | Out-Null
@(
  "### clang-tidy warnings/errors (0)",
  "",
  "No clang-tidy warnings or errors.",
  "",
  "### compiler warnings/errors (0)",
  "",
  "No compiler warnings or errors."
) | Out-File -FilePath $reportPath -Encoding utf8

$prBaseSha = $env:INPUT_PR_BASE_SHA
$prHeadSha = $env:INPUT_PR_HEAD_SHA
if (-not [string]::IsNullOrWhiteSpace($prBaseSha) -and -not [string]::IsNullOrWhiteSpace($prHeadSha)) {
  $base = $prBaseSha
  $head = $prHeadSha
  $diffRange = "$base...$head"
  Write-Host "Computing clang-tidy candidates for PR diff (caller-provided SHAs): $diffRange"
} elseif ($env:GITHUB_EVENT_NAME -eq "pull_request") {
  $base = $env:EVENT_PR_BASE_SHA
  $head = $env:EVENT_PR_HEAD_SHA
  $diffRange = "$base...$head"
  Write-Host "Computing clang-tidy candidates for PR diff (event payload): $diffRange"
} else {
  $base = $env:EVENT_BEFORE_SHA
  if (-not $base -or $base -match '^0+$') {
    $base = "HEAD~1"
  }
  $head = $env:EVENT_SHA
  $diffRange = "$base..$head"
  Write-Host "Computing clang-tidy candidates for push diff: $diffRange"
}

$changedLinesByFile = @{}
$currentFile = $null
$diffLines = & git diff --unified=0 --no-color --no-prefix --diff-filter=ACMR $diffRange -- client engine shared
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

$allChangedFiles = @(
  & git diff --name-only --diff-filter=ACMR $diffRange -- client engine shared |
    Sort-Object -Unique
)
Write-Host ("clang-tidy debug: changed files in scope: {0}" -f $allChangedFiles.Count)
$allChangedFiles | ForEach-Object { Write-Host " - changed: $_" }
Write-Host ("clang-tidy debug: changed files with tracked line hunks: {0}" -f $changedLinesByFile.Keys.Count)
foreach ($changedKey in ($changedLinesByFile.Keys | Sort-Object)) {
  Write-Host (" - changed-lines: {0} ({1} lines)" -f $changedKey, $changedLinesByFile[$changedKey].Count)
}

# TODO: Lint bgfx_renderer_bgfx.cpp once a Windows CI runner is available or
# the file supports cross-platform compilation.
# bgfx_renderer_bgfx.cpp is Windows-only (#error on other platforms); skip on Linux CI.
$files = @(
  & git diff --name-only --diff-filter=ACMR $diffRange -- client engine shared |
    Where-Object {
      # TODO: Re-introduce changed .hpp lint coverage via generated
      # translation-unit wrappers instead of direct header-mode runs.
      ($_ -like "*.cpp") -and
      $_ -notlike "*bgfx_renderer_bgfx.cpp"
    } |
    Sort-Object -Unique
)

Write-Host ("clang-tidy candidate files: {0}" -f $files.Count)
$files | ForEach-Object { Write-Host " - $_" }
$cppFiles = @($files | Where-Object { $_ -like "*.cpp" })
$headerFiles = @($files | Where-Object { $_ -like "*.hpp" })
Write-Host ("clang-tidy debug: compile_commands candidates (.cpp): {0}" -f $cppFiles.Count)
Write-Host ("clang-tidy debug: fallback candidates (.hpp): {0}" -f $headerFiles.Count)

if (-not $files) {
  "### clang diagnostics (0)" | Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Append -Encoding utf8
  @(
    "### clang-tidy warnings/errors (0)",
    "",
    "No clang-tidy warnings or errors.",
    "",
    "### compiler warnings/errors (0)",
    "",
    "No compiler warnings or errors."
  ) | Out-File -FilePath $reportPath -Encoding utf8
  "warnings_count=0" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  "errors_count=0" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  "diagnostics_count=0" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
  exit 0
}

$executionRoot = (& bazel info @bazelFlags execution_root).Trim()
$outputBase = (& bazel info @bazelFlags output_base).Trim()

$externalRoots = @(
  (Join-Path $executionRoot "external"),
  (Join-Path $outputBase "external")
) | Where-Object { Test-Path $_ }

function Find-ExternalRepo([string]$pattern) {
  foreach ($root in $externalRoots) {
    $match = Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
      Where-Object { $_.Name -like $pattern } |
      Select-Object -First 1
    if ($match) { return $match }
  }
  return $null
}

$gtestRepo = $null
$bgfxRepo = $null
$bxRepo = $null
$sdlRepo = $null
$abslRepo = $null
$rulesCcRepo = $null
$needsGtest = $false
$needsBgfxHeaders = $false
$needsRulesCcRunfiles = $false
foreach ($file in $files) {
  if ($file -match '_test\.(cpp|hpp)$') {
    $needsGtest = $true
  }
  $fullPath = Join-Path $workspace $file
  if ((-not $needsBgfxHeaders) -and (Test-Path $fullPath)) {
    $needsBgfxHeaders = Select-String -Path $fullPath -Pattern '#\s*include\s*<bgfx/' -Quiet
  }
  if ((-not $needsRulesCcRunfiles) -and (Test-Path $fullPath)) {
    $needsRulesCcRunfiles = Select-String -Path $fullPath -Pattern '#\s*include\s*"rules_cc/cc/runfiles/runfiles.h"|#\s*include\s*"tools/cpp/runfiles/runfiles.h"' -Quiet
  }
}

$sdlRepo = Find-ExternalRepo "*sdl3_source*"
if (-not $sdlRepo) {
  throw "Could not locate SDL3 external repository (expected: sdl3_source) under any of: $($externalRoots -join ', ')"
}
$abslRepo = Find-ExternalRepo "*abseil-cpp*"
if (-not $abslRepo) {
  Write-Host "abseil-cpp externals not present yet; fetching via Bazel."
  & bazel fetch @bazelFlags //engine/src:render
  $abslRepo = Find-ExternalRepo "*abseil-cpp*"
}
if (-not $abslRepo) {
  throw "Could not locate abseil-cpp external repository under any of: $($externalRoots -join ', ')"
}

if ($needsGtest) {
  $gtestRepo = Find-ExternalRepo "*googletest*"
  if (-not $gtestRepo) {
    Write-Host "googletest externals not present yet; fetching via Bazel."
    & bazel fetch @bazelFlags //engine/src/render:overlay_transparency_contract_test
    $gtestRepo = Find-ExternalRepo "*googletest*"
  }
  if (-not $gtestRepo) {
    throw "Could not locate googletest external repository under any of: $($externalRoots -join ', ')"
  }
}

if ($needsBgfxHeaders) {
  $bgfxRepo = Find-ExternalRepo "*bgfx_upstream*"
  $bxRepo = Find-ExternalRepo "*bx_upstream*"
  if ((-not $bgfxRepo) -or (-not $bxRepo)) {
    Write-Host "bgfx/bx externals not present yet; fetching via Bazel."
    & bazel fetch @bazelFlags @bgfx_upstream//:bgfx
    $bgfxRepo = Find-ExternalRepo "*bgfx_upstream*"
    $bxRepo = Find-ExternalRepo "*bx_upstream*"
  }
  if (-not $bgfxRepo) {
    throw "Could not locate bgfx_upstream external repository under any of: $($externalRoots -join ', ')"
  }
  if (-not $bxRepo) {
    throw "Could not locate bx_upstream external repository under any of: $($externalRoots -join ', ')"
  }
}

if ($needsRulesCcRunfiles) {
  $rulesCcRepo = Find-ExternalRepo "*rules_cc*"
  if (-not $rulesCcRepo) {
    Write-Host "rules_cc externals not present yet; fetching via Bazel."
    & bazel fetch @bazelFlags @rules_cc//cc/runfiles
    $rulesCcRepo = Find-ExternalRepo "*rules_cc*"
  }
  if (-not $rulesCcRepo) {
    throw "Could not locate rules_cc external repository under any of: $($externalRoots -join ', ')"
  }
}

$sdlRepoPath = if ($sdlRepo) { $sdlRepo.FullName } else { "" }
$abslRepoPath = if ($abslRepo) { $abslRepo.FullName } else { "" }
$gtestRepoPath = if ($gtestRepo) { $gtestRepo.FullName } else { "" }
$bgfxRepoPath = if ($bgfxRepo) { $bgfxRepo.FullName } else { "" }
$bxRepoPath = if ($bxRepo) { $bxRepo.FullName } else { "" }
$rulesCcRepoPath = if ($rulesCcRepo) { $rulesCcRepo.FullName } else { "" }

$hadErrors = $false
Write-Host "clang-tidy debug: begin clang-tidy execution"
$results = $files | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
  $file = $_
  $w = $using:workspace
  $fullFile = Join-Path $w $file
  $sdlPath = $using:sdlRepoPath
  $abslPath = $using:abslRepoPath
  $gtestPath = $using:gtestRepoPath
  $bgfxPath = $using:bgfxRepoPath
  $bxPath = $using:bxRepoPath
  $rulesCcPath = $using:rulesCcRepoPath

  $clangArgs = @("--quiet", "--header-filter=^(client|engine|shared)/", $fullFile)
  if ($file -like "*.cpp") {
    $clangArgs += @(
      "-p", $w,
      "--extra-arg=-I$w",
      "--extra-arg=-I$w/engine/include",
      "--extra-arg=-I$w/shared/include"
    )
    if ($sdlPath) {
      $clangArgs += "--extra-arg=-isystem$sdlPath/include"
    }
    if ($abslPath) {
      $clangArgs += "--extra-arg=-isystem$abslPath"
    }
    if ($bgfxPath) {
      $clangArgs += "--extra-arg=-isystem$bgfxPath/include"
      $clangArgs += "--extra-arg=-isystem$bgfxPath/3rdparty"
      $clangArgs += "--extra-arg=-isystem$bgfxPath/3rdparty/directx-headers/include"
      $clangArgs += "--extra-arg=-isystem$bgfxPath/3rdparty/directx-headers/include/directx"
      $clangArgs += "--extra-arg=-isystem$bgfxPath/3rdparty/khronos"
    }
    if ($bxPath) {
      $clangArgs += "--extra-arg=-isystem$bxPath/include"
      $clangArgs += "--extra-arg=-isystem$bxPath/3rdparty"
    }
    if ($rulesCcPath) {
      $clangArgs += "--extra-arg=-isystem$rulesCcPath"
    }
    if ($file -match '_test\.(cpp|hpp)$' -and $gtestPath) {
      $clangArgs += "--extra-arg=-isystem$gtestPath"
      $clangArgs += "--extra-arg=-isystem$gtestPath/googletest/include"
      $clangArgs += "--extra-arg=-isystem$gtestPath/googlemock/include"
    }
  } else {
    $clangArgs += @(
      "--"
      "-std=c++20"
      "-I$w"
      "-I$w/engine/include"
      "-I$w/shared/include"
      "-isystem$sdlPath/include"
      "-isystem$abslPath"
    )
    if ($bgfxPath) {
      $clangArgs += "-isystem$bgfxPath/include"
      $clangArgs += "-isystem$bgfxPath/3rdparty"
      $clangArgs += "-isystem$bgfxPath/3rdparty/directx-headers/include"
      $clangArgs += "-isystem$bgfxPath/3rdparty/directx-headers/include/directx"
      $clangArgs += "-isystem$bgfxPath/3rdparty/khronos"
    }
    if ($bxPath) {
      $clangArgs += "-isystem$bxPath/include"
      $clangArgs += "-isystem$bxPath/3rdparty"
    }
    if ($rulesCcPath) {
      $clangArgs += "-isystem$rulesCcPath"
    }
    if ($file -match '_test\.(cpp|hpp)$' -and $gtestPath) {
      $clangArgs += "-isystem$gtestPath"
      $clangArgs += "-isystem$gtestPath/googletest/include"
      $clangArgs += "-isystem$gtestPath/googlemock/include"
    }
  }

  $output = & clang-tidy @clangArgs 2>&1
  $outputStrings = if ($null -ne $output) {
    $output | ForEach-Object { $_.ToString() }
  } else { @() }

  return [pscustomobject]@{
    File = $file
    Output = $outputStrings
    ExitCode = $LASTEXITCODE
  }
}

foreach ($res in $results) {
  if ($null -ne $res.Output -and $res.Output.Count -gt 0) {
    Write-Host ("clang-tidy debug: captured output lines for {0}: {1}" -f $res.File, $res.Output.Count)
    $res.Output | Out-File -FilePath $logPath -Append -Encoding utf8
  }
  if ($res.ExitCode -ne 0) {
    $hadErrors = $true
  }
}
Write-Host "clang-tidy debug: finished clang-tidy execution"

Write-Host "clang-tidy debug: begin parse and report"
try {
  # Some toolchains omit column numbers (file:line: warning: ...). Accept both
  # file:line:col and file:line diagnostic formats.
  $diagnosticPattern = '^(?<file>(?:[A-Za-z]:)?[^:]+):(?<line>\d+)(?::(?<col>\d+))?:\s+(?<severity>warning|error):\s+(?<msg>.*?)(?:\s+\[(?<check>[^\]]+)\])?$'
  $diagnosticPatternSmokeLines = @(
    "client/src/smoke.cpp:1:2: warning: warning-smoke [misc-smoke]",
    "client/src/smoke.cpp:3:4: error: error-smoke [clang-diagnostic-error]"
  )
  foreach ($smokeLine in $diagnosticPatternSmokeLines) {
    if (-not ($smokeLine -match $diagnosticPattern)) {
      throw "Diagnostic parser smoke test failed for line: $smokeLine"
    }
  }
  $tidyWarnings = @()
  $compilerWarnings = @()
  $tidyErrors = @()
  $compilerErrors = @()
  $tidyWarningCount = 0
  $compilerWarningCount = 0
  $tidyErrorCount = 0
  $compilerErrorCount = 0
  $parsedDiagnosticsCount = 0
  $includedDiagnosticsCount = 0
  $excludedDiagnosticsCount = 0
  $excludedMissingChangedLineCount = 0
  $unparsedWarningErrorLineCount = 0
  $unparsedWarningErrorSamples = New-Object System.Collections.Generic.List[string]
  $diagnosticFilesSeen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
  $excludedFilesSeen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
  $workspaceNorm = [System.IO.Path]::GetFullPath($workspace).TrimEnd([char[]]@('\', '/'))
  $changedFiles = @($changedLinesByFile.Keys | Sort-Object { $_.Length } -Descending)
  [array]$logLines = if (Test-Path $logPath) { Get-Content $logPath } else { @() }
  foreach ($line in $logLines) {
    $cleanLine = $line -replace "$([char]27)\[[0-9;]*[A-Za-z]", ""
    if ($cleanLine -match $diagnosticPattern) {
      try {
        $parsedDiagnosticsCount += 1
      $rawFile = $Matches.file
      $rawFileNorm = ($rawFile -replace '\\', '/').Trim()
      $rawFilePath = $rawFileNorm.Replace('/', '\')
      $file = $rawFileNorm
      $fullFile = $null

    if ([System.IO.Path]::IsPathRooted($rawFilePath)) {
      $fullFile = [System.IO.Path]::GetFullPath($rawFilePath)
      $fullFileNorm = $fullFile -replace '\\', '/'

      # Prefer changed-file keys (same style as format report: repo-relative paths).
      $matchedChanged = $null
      foreach ($candidate in $changedFiles) {
        if ($fullFileNorm.EndsWith("/$candidate", [System.StringComparison]::OrdinalIgnoreCase) -or
            $fullFileNorm.Equals($candidate, [System.StringComparison]::OrdinalIgnoreCase)) {
          $matchedChanged = $candidate
          break
        }

        # Bazel virtual include paths often look like:
        # .../_virtual_includes/<target>/isla/engine/.../file.hpp
        # Map those back to changed workspace headers such as:
        # engine/include/isla/engine/.../file.hpp
        $candidateIncludeMarker = "/include/"
        $candidateNorm = $candidate -replace '\\', '/'
        $includeIndex = $candidateNorm.IndexOf($candidateIncludeMarker, [System.StringComparison]::OrdinalIgnoreCase)
        if ($includeIndex -ge 0) {
          $includeSuffixStart = $includeIndex + $candidateIncludeMarker.Length
          if ($includeSuffixStart -lt $candidateNorm.Length) {
            $includeSuffix = $candidateNorm.Substring($includeSuffixStart)
            if ($fullFileNorm.EndsWith("/$includeSuffix", [System.StringComparison]::OrdinalIgnoreCase)) {
              $matchedChanged = $candidate
              break
            }
          }
        }
      }

      if ($matchedChanged) {
        $file = $matchedChanged
      } else {
        $workspaceNormUnix = $workspaceNorm -replace '\\', '/'
        if (
          $fullFileNorm.Equals($workspaceNormUnix, [System.StringComparison]::OrdinalIgnoreCase) -or
          $fullFileNorm.StartsWith($workspaceNormUnix + "/", [System.StringComparison]::OrdinalIgnoreCase)
        ) {
          $file = $fullFileNorm.Substring($workspaceNormUnix.Length).TrimStart([char[]]@('\', '/'))
        } else {
          $file = $fullFileNorm
        }
      }
    }

    $line = $Matches.line
    $col = if ($Matches.col) { $Matches.col } else { "1" }
    $severity = if ($Matches.severity) { $Matches.severity.Trim().ToLowerInvariant() } else { "warning" }
    $check = if ($Matches.check) { $Matches.check.Trim() } else { "" }
    $checkLabel = if ($check) { $check } else { "clang-diagnostic" }
    $rawReason = if ($Matches.msg) { $Matches.msg.Trim() } else { "" }

    $file = ($file -replace '\\', '/')
    $pathMatch = [regex]::Match($file, '(?<rel>(?:client|engine|shared)/.+)$')
    if ($pathMatch.Success) {
      $file = $pathMatch.Groups['rel'].Value
    }
    $null = $diagnosticFilesSeen.Add($file)
    if ([string]::IsNullOrWhiteSpace($rawReason)) {
      $rawReason = "No diagnostic message provided by clang output."
    }
    $isTidyWarning = (
      -not [string]::IsNullOrWhiteSpace($check) -and
      -not $check.StartsWith("-W") -and
      -not $check.StartsWith("clang-diagnostic-")
    )
    $isTestFile = $file -match '(_test|_tests|\.spec)\.cpp$'
    if ($isTestFile -and ($check -eq 'cppcoreguidelines-avoid-non-const-global-variables' -or $check -eq 'readability-magic-numbers')) {
      continue
    }
    $msg = "$rawReason [$checkLabel]"
    $safeMsg = $msg -replace '%', '%25' -replace '\r', '%0D' -replace '\n', '%0A'
    $reportItem = @(
      "<details>",
      ('<summary><code>{0}:{1}:{2}</code> [{3}]</summary>' -f $file, $line, $col, $checkLabel),
      "",
      ('**reason:** `{0}`' -f $rawReason),
      "",
      "</details>",
      ""
    )

    $lineNumber = [int]$line
    $isChangedLine = $changedLinesByFile.ContainsKey($file) -and $changedLinesByFile[$file].Contains($lineNumber)
    # Intentionally report warnings only on changed lines to keep PR feedback
    # tightly scoped; include all errors regardless of line ownership.
    $includeDiagnostic = $isChangedLine -or ($severity -eq "error")
      if ($includeDiagnostic) {
        $includedDiagnosticsCount += 1
        if ($severity -eq "error") {
          if ($isTidyWarning) {
          Write-Host ("clang-tidy diag error: {0}:{1}:{2} {3}" -f $file, $line, $col, $checkLabel)
          $tidyErrors += $reportItem
          $tidyErrorCount += 1
        } else {
          Write-Host ("clang compiler error: {0}:{1}:{2} {3}" -f $file, $line, $col, $checkLabel)
          $compilerErrors += $reportItem
          $compilerErrorCount += 1
        }
      } elseif ($isChangedLine) {
        if ($isTidyWarning) {
          Write-Host ("clang-tidy diag warning: {0}:{1}:{2} {3}" -f $file, $line, $col, $checkLabel)
          $tidyWarnings += $reportItem
          $tidyWarningCount += 1
        } else {
          Write-Host ("clang compiler warning: {0}:{1}:{2} {3}" -f $file, $line, $col, $checkLabel)
          $compilerWarnings += $reportItem
          $compilerWarningCount += 1
        }
      }
    } else {
      $excludedDiagnosticsCount += 1
      if (-not $isChangedLine -and $severity -ne "error") {
        $excludedMissingChangedLineCount += 1
      }
      $null = $excludedFilesSeen.Add($file)
    }
      } catch {
        $lineParseError = $_
        $lineParseMessage = if ($lineParseError -and $lineParseError.Exception) {
          $lineParseError.Exception.Message
        } else {
          "unknown per-line parse failure"
        }
        Write-Host ("clang-tidy debug: per-line parse failure: {0}" -f $lineParseMessage)
      }
    } elseif ($cleanLine -match ':\s*(warning|error):') {
      $unparsedWarningErrorLineCount += 1
      if ($unparsedWarningErrorSamples.Count -lt 10) {
        $unparsedWarningErrorSamples.Add($cleanLine)
      }
    }
  }
} catch {
  $parseError = $_
  $parseErrorMessage = if ($parseError -and $parseError.Exception) {
    $parseError.Exception.Message
  } else {
    "unknown parse/report phase failure"
  }
  @(
    "### clang-tidy warnings/errors (0)",
    "",
    "clang-tidy parse/report phase failed.",
    "",
    "### compiler warnings/errors (0)",
    "",
    "clang-tidy parse/report phase failed."
  ) | Out-File -FilePath $reportPath -Encoding utf8
  Write-Host "::error::clang-tidy parse/report phase failed: $parseErrorMessage"
  throw
}
Write-Host "clang-tidy debug: finished parse and report"

Write-Host ("clang-tidy debug: parsed diagnostics: {0}" -f $parsedDiagnosticsCount)
Write-Host ("clang-tidy debug: included diagnostics: {0}" -f $includedDiagnosticsCount)
Write-Host ("clang-tidy debug: excluded diagnostics: {0}" -f $excludedDiagnosticsCount)
Write-Host ("clang-tidy debug: excluded (warning not on changed line): {0}" -f $excludedMissingChangedLineCount)
Write-Host ("clang-tidy debug: unparsed warning/error lines: {0}" -f $unparsedWarningErrorLineCount)
$unparsedWarningErrorSamples | ForEach-Object { Write-Host " - unparsed-sample: $_" }
Write-Host ("clang-tidy debug: diagnostic files seen: {0}" -f $diagnosticFilesSeen.Count)
$diagnosticFilesSeen | Sort-Object | ForEach-Object { Write-Host " - diagnostic-file: $_" }
Write-Host ("clang-tidy debug: excluded files seen: {0}" -f $excludedFilesSeen.Count)
$excludedFilesSeen | Sort-Object | ForEach-Object { Write-Host " - excluded-file: $_" }

$totalErrors = $tidyErrorCount + $compilerErrorCount
$totalWarnings = $tidyWarningCount + $compilerWarningCount
$totalDiagnostics = $totalWarnings + $totalErrors
$tidyDiagnosticCount = $tidyWarningCount + $tidyErrorCount
$compilerDiagnosticCount = $compilerWarningCount + $compilerErrorCount
$summaryLines = @(
  ("### clang diagnostics ({0})" -f $totalDiagnostics),
  "",
  "<details>",
  ("<summary>clang-tidy warnings/errors ({0})</summary>" -f $tidyDiagnosticCount),
  ""
)
if ($tidyDiagnosticCount -gt 0) {
  $summaryLines += $tidyWarnings
  $summaryLines += $tidyErrors
} else {
  $summaryLines += "No clang-tidy warnings or errors."
}
$summaryLines += @(
  "",
  "</details>",
  "",
  "<details>",
  ("<summary>compiler warnings/errors ({0})</summary>" -f $compilerDiagnosticCount),
  ""
)
if ($compilerDiagnosticCount -gt 0) {
  $summaryLines += $compilerWarnings
  $summaryLines += $compilerErrors
} else {
  $summaryLines += "No compiler warnings or errors."
}
$summaryLines += @(
  "",
  "</details>"
)

$prReportLines = @(
  ("### clang-tidy warnings/errors ({0})" -f $tidyDiagnosticCount),
  ""
)
if ($tidyDiagnosticCount -gt 0) {
  $prReportLines += $tidyWarnings
  $prReportLines += $tidyErrors
} else {
  $prReportLines += "No clang-tidy warnings or errors."
}
$prReportLines += @(
  "",
  ("### compiler warnings/errors ({0})" -f $compilerDiagnosticCount),
  ""
)
if ($compilerDiagnosticCount -gt 0) {
  $prReportLines += $compilerWarnings
  $prReportLines += $compilerErrors
} else {
  $prReportLines += "No compiler warnings or errors."
}

$summaryLines | Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Append -Encoding utf8
$prReportLines | Out-File -FilePath $reportPath -Encoding utf8
"warnings_count=$totalWarnings" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
"errors_count=$totalErrors" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
"diagnostics_count=$totalDiagnostics" | Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
Write-Host ("clang-tidy debug: wrote report file to {0}" -f $reportPath)
if (Test-Path $reportPath) {
  Write-Host "clang-tidy debug: report preview (first 20 lines)"
  Get-Content -Path $reportPath | Select-Object -First 20 | ForEach-Object { Write-Host $_ }
} else {
  Write-Host "::error::clang-tidy debug: expected report file missing at $reportPath"
}
Write-Host ("clang-tidy debug: final counts warnings={0} errors={1} total={2}" -f $totalWarnings, $totalErrors, $totalDiagnostics)

if ($hadErrors) {
  Write-Host "clang-tidy debug: exiting with failure because at least one clang-tidy invocation returned non-zero"
  exit 1
}
Write-Host "clang-tidy debug: completed step successfully"
