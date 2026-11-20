@echo off
setlocal
pushd "%~dp0"
if errorlevel 1 exit /b 1

where gh >nul 2>nul
if errorlevel 1 (
    echo ERROR: GitHub CLI is required. Install gh and run gh auth login.
    goto failed
)

for %%F in ("build\milton.exe" "build\Carlito.ttf" "build\Carlito.LICENSE" "LICENSE.txt" "milton_icon.ico") do (
    if not exist "%%~F" (
        echo ERROR: Required package file %%~F does not exist. Build Milton first.
        goto failed
    )
)

set "version="
for /f %%V in ('powershell.exe -NoProfile -Command "Get-Date -Format yyyyMMdd.HHmmss"') do set "version=%%V"
if not defined version (
    echo ERROR: Could not generate the release timestamp.
    goto failed
)

set "archive=build\milton-%version%.zip"
powershell.exe -NoProfile -Command "$ErrorActionPreference = 'Stop'; Compress-Archive -LiteralPath 'build\milton.exe', 'build\Carlito.ttf', 'build\Carlito.LICENSE', 'LICENSE.txt', 'milton_icon.ico' -DestinationPath $env:archive"
if errorlevel 1 (
    echo ERROR: Could not create the release ZIP.
    goto failed
)

call gh release create "%version%" "%archive%" --repo DimFann/milton --draft --title "%version%" --notes-file NUL
if errorlevel 1 (
    echo ERROR: Draft release creation or asset upload failed. Check GitHub for a partial draft before retrying.
    goto failed
)

popd
exit /b 0

:failed
popd
exit /b 1
