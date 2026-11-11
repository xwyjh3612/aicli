param(
    [Parameter(Mandatory = $true)]
    [string]$InstallDirectory,
    [switch]$IncludeProcessTrees
)

$ErrorActionPreference = 'Stop'

$targetPath = [IO.Path]::GetFullPath(
    (Join-Path $InstallDirectory 'x64\OpenConsole.exe'))

function Stop-NativeProcessTrees {
    $appPath = [IO.Path]::GetFullPath((Join-Path $InstallDirectory 'aicli.exe'))
    $snapshot = @(Get-CimInstance Win32_Process -ErrorAction Stop)
    $records = @{}
    $roots = @{}
    foreach ($item in $snapshot) {
        $id = [int]$item.ProcessId
        $records[$id] = $item
        if ([string]::IsNullOrWhiteSpace($item.ExecutablePath)) { continue }
        $path = [IO.Path]::GetFullPath($item.ExecutablePath)
        if ([string]::Equals($path, $appPath, [StringComparison]::OrdinalIgnoreCase) -or
            [string]::Equals($path, $targetPath, [StringComparison]::OrdinalIgnoreCase)) {
            $roots[$id] = $true
        }
    }

    # The updater may itself have been launched by AiCli. Protect its branch,
    # including this PowerShell helper, while still closing the AiCli ancestor.
    $lineage = @{}
    $ancestorId = [int]$PID
    while ($records.ContainsKey($ancestorId) -and !$roots.ContainsKey($ancestorId) -and
           !$lineage.ContainsKey($ancestorId)) {
        $lineage[$ancestorId] = $true
        $ancestorId = [int]$records[$ancestorId].ParentProcessId
    }
    $protected = @{ ([int]$PID) = $true }
    if ($roots.ContainsKey($ancestorId)) { $protected = $lineage }
    do {
        $changed = $false
        foreach ($item in $snapshot) {
            $id = [int]$item.ProcessId
            $parentId = [int]$item.ParentProcessId
            if (!$protected.ContainsKey($id) -and $protected.ContainsKey($parentId) -and
                $records[$parentId].CreationDate -le $item.CreationDate) {
                $protected[$id] = $true
                $changed = $true
            }
        }
    } while ($changed)

    # Retain real handles, not only PIDs. PID reuse must never cause a different
    # user's process to be terminated during an upgrade.
    $owned = @{}
    $handles = @{}
    $stopping = $false
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            $current = @{}
            foreach ($item in $snapshot) { $current[[int]$item.ProcessId] = $item }
            do {
                $changed = $false
                foreach ($item in $snapshot) {
                    $id = [int]$item.ProcessId
                    $parentId = [int]$item.ParentProcessId
                    if ($owned.ContainsKey($id) -or $protected.ContainsKey($id)) { continue }
                    $isOwned = $roots.ContainsKey($id) -and
                        $records[$id].CreationDate -eq $item.CreationDate
                    if (!$isOwned -and $owned.ContainsKey($parentId)) {
                        $parent = $owned[$parentId]
                        $isOwned = $parent.CreationDate -le $item.CreationDate
                        if ($current.ContainsKey($parentId) -and
                            $current[$parentId].CreationDate -ne $parent.CreationDate) {
                            $isOwned = $false
                        }
                    }
                    if (!$isOwned) { continue }
                    try {
                        $live = [Diagnostics.Process]::GetProcessById($id)
                        $null = $live.Handle
                        $createdAt = $live.StartTime.ToUniversalTime()
                        if ([Math]::Abs(($createdAt - $item.CreationDate.ToUniversalTime()).TotalMilliseconds) -gt 1) {
                            $live.Dispose()
                            continue
                        }
                        $handles[$id] = $live
                    } catch [ArgumentException] {
                        # Already exited. Its captured ancestry still identifies
                        # descendants in this snapshot.
                    }
                    $owned[$id] = $item
                    $changed = $true
                }
            } while ($changed)

            # Capture again before stopping parents to catch children created
            # while CIM was collecting the initial snapshot.
            if ($changed -eq $false -and !$stopping) {
                $stopping = $true
                $snapshot = @(Get-CimInstance Win32_Process -ErrorAction Stop)
                continue
            }
            foreach ($id in @($roots.Keys) + @($handles.Keys)) {
                if ($handles.ContainsKey($id)) {
                    $live = $handles[$id]
                    if (!$live.HasExited) {
                        try { $live.Kill() } catch {
                            # Terminating an ancestor can already have started
                            # this child's job shutdown. Windows may report
                            # access denied until that exit finishes.
                            if (!$live.WaitForExit(1000)) { throw }
                        }
                    }
                }
            }
            $snapshot = @(Get-CimInstance Win32_Process -ErrorAction Stop)
            $remaining = @($handles.Values | Where-Object { !$_.HasExited })
            # Also discover any last descendants of an exited parent before
            # declaring success; the original process handles remain held.
            $newDescendants = @($snapshot | Where-Object {
                !$owned.ContainsKey([int]$_.ProcessId) -and
                !$protected.ContainsKey([int]$_.ProcessId) -and
                $owned.ContainsKey([int]$_.ParentProcessId) -and
                $owned[[int]$_.ParentProcessId].CreationDate -le $_.CreationDate
            })
            if ($remaining.Count -eq 0 -and $newDescendants.Count -eq 0) { return $true }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $deadline)
        return $false
    } finally {
        foreach ($live in $handles.Values) { $live.Dispose() }
    }
}

if ($IncludeProcessTrees) {
    try {
        if (Stop-NativeProcessTrees) { exit 0 }
    } catch {
        Write-Error ('Cannot close AiCli process tree for this installation: ' + $_.Exception.Message) -ErrorAction Continue
    }
    exit 1
}

function Get-MatchingOpenConsoleProcesses {
    @(
        Get-CimInstance Win32_Process -Filter "Name = 'OpenConsole.exe'" `
            -ErrorAction SilentlyContinue |
            Where-Object {
                if ([string]::IsNullOrWhiteSpace($_.ExecutablePath)) {
                    return $false
                }

                try {
                    $processPath = [IO.Path]::GetFullPath($_.ExecutablePath)
                    return [string]::Equals(
                        $processPath,
                        $targetPath,
                        [StringComparison]::OrdinalIgnoreCase)
                } catch {
                    return $false
                }
            }
    )
}

foreach ($process in (Get-MatchingOpenConsoleProcesses)) {
    Stop-Process -Id $process.ProcessId -Force -ErrorAction SilentlyContinue
}

$deadline = [DateTime]::UtcNow.AddSeconds(5)
do {
    $remaining = @(Get-MatchingOpenConsoleProcesses)
    if ($remaining.Count -eq 0) {
        exit 0
    }
    Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $deadline)

exit 1
