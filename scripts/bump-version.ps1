param(
    [string]$VersionFile = "VERSION"
)

$raw = (Get-Content -LiteralPath $VersionFile -TotalCount 1).Trim()
if ($raw -notmatch '^(\d+)\.(\d+)\.(\d+)$') {
    throw "VERSION must be major.minor.patch, got '$raw'"
}

$major = [int]$Matches[1]
$minor = [int]$Matches[2]
$patch = [int]$Matches[3] + 1
$next = "$major.$minor.$patch"

Set-Content -LiteralPath $VersionFile -Value $next -NoNewline
Write-Output $next
