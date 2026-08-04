# Removes Godot/Windows temp GDExtension artifacts (~*.dll, *~RF*.TMP) that can break
# extension loading with "Failed to open '~...dll'" / ERR_FILE_NOT_FOUND cascades.
# Close all Godot editors and exported games using these DLLs before running.

$ErrorActionPreference = "Continue"
$roots = @(
    Join-Path $PSScriptRoot "..\addons\nexus_resonance\bin\windows",
    Join-Path $PSScriptRoot "..\project\addons\nexus_resonance\bin\windows"
)
foreach ($d in $roots) {
    $full = [System.IO.Path]::GetFullPath($d)
    if (-not (Test-Path -LiteralPath $full)) { continue }
    Get-ChildItem -LiteralPath $full -Force -File | Where-Object {
        $_.Name -like "~*" -or $_.Name -like "*~RF*.TMP"
    } | ForEach-Object {
        Remove-Item -LiteralPath $_.FullName -Force
        Write-Host "Removed: $($_.FullName)"
    }
}
Write-Host "Done."
