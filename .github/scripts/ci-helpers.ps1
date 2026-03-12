function Get-CiExcludedPathPrefixes() {
  return @(
    "third_party/",
    "external/",
    "tools/",
    "bazel-",
    ".git/",
    ".github/"
  )
}

function Convert-ToNormalizedRepoPath([string]$path) {
  return ($path -replace '\\', '/').Trim()
}

function Test-IsExcludedCiPath([string]$path) {
  $normalized = Convert-ToNormalizedRepoPath $path
  foreach ($prefix in Get-CiExcludedPathPrefixes) {
    if ($normalized.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
      return $true
    }
  }
  return $false
}

function Test-IsFirstPartyCppPath([string]$path) {
  $normalized = Convert-ToNormalizedRepoPath $path
  if ([string]::IsNullOrWhiteSpace($normalized) -or (Test-IsExcludedCiPath $normalized)) {
    return $false
  }

  return (
    $normalized -like "*.cpp" -or
    $normalized -like "*.hpp" -or
    $normalized -like "*.h"
  )
}

function New-FirstPartyHeaderFilter([string]$workspacePath) {
  $normalizedWorkspace = Convert-ToNormalizedRepoPath $workspacePath
  $escapedWorkspace = [regex]::Escape($normalizedWorkspace)
  $excludedAlternation = ((Get-CiExcludedPathPrefixes) | ForEach-Object {
      [regex]::Escape($_)
    }) -join "|"
  return "^(?:" + $escapedWorkspace + "/)?(?!$excludedAlternation).+"
}
