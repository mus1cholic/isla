$flags = @()
if ($env:BUILDBUDDY_API_KEY) {
  Write-Host "Using BuildBuddy Remote Cache with --config=remote"
  $flags += "--config=remote"
  $flags += "--remote_header=x-buildbuddy-api-key=$env:BUILDBUDDY_API_KEY"
}

$flags
