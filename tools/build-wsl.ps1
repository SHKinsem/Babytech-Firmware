param(
    [ValidateSet('all', 'brain', 'motion', 'test')]
    [string]$Target = 'all',
    [string]$Distro = 'Ubuntu',
    [ValidateRange(1, 32)]
    [int]$Jobs = 8
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path.Replace('\', '/')
$script = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'build-wsl.sh') -Raw
# Pass paths as positional arguments; never interpolate them into shell code.
# PowerShell appends CRLF to piped input; exit before Bash reads that extra line.
($script.Replace("`r`n", "`n").TrimEnd() + "`nexit 0`n") | & wsl.exe -d $Distro --exec bash -s -- $projectRoot $Target $Jobs
$buildExit = $LASTEXITCODE
if ($buildExit -ne 0) { throw "WSL build failed with exit code $buildExit" }
