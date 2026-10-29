param(
    [Parameter(Mandatory = $true)]
    [string]$Directory,
    [Parameter(Mandatory = $true)]
    [string]$Files
)

# Writes SHA256SUMS.txt into Directory for the comma-separated file names in Files.
# The format matches sha256sum, so `sha256sum -c SHA256SUMS.txt` verifies it.

$ErrorActionPreference = 'Stop'

[System.Collections.Generic.List[string]]$lines = New-Object 'System.Collections.Generic.List[string]'

foreach ($name in $Files.Split(',')) {
    [string]$fileName = $name.Trim()
    if ($fileName -eq '') {
        continue
    }

    [string]$path = Join-Path $Directory $fileName
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "File to checksum not found: $path"
    }

    [string]$hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $lines.Add("$hash  $fileName")
}

[string]$target = Join-Path $Directory 'SHA256SUMS.txt'
[System.IO.File]::WriteAllLines($target, $lines.ToArray(), (New-Object System.Text.UTF8Encoding $false))
Write-Output "Wrote $target"
