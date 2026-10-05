@echo off

setlocal

set "platform=x64"

if /i "%~1"=="x86" set "platform=x86"

set "initializeMsvc="
where cl >nul 2>nul
if errorlevel 1 set "initializeMsvc=1"
where rc >nul 2>nul
if errorlevel 1 set "initializeMsvc=1"
if defined initializeMsvc goto initialize_msvc
goto compiler_ready

:initialize_msvc
set "vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%vswhere%" (
   echo ERROR: Visual Studio C++ tools were not found. Install the Visual Studio C++ workload or run this from a Developer Command Prompt.
   exit /b 1
)

set "vsInstallPath="
for /f "usebackq tokens=*" %%i in (`"%vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "vsInstallPath=%%i"
if not defined vsInstallPath (
   echo ERROR: No Visual Studio installation with the C++ build tools was found.
   exit /b 1
)

if not exist "%vsInstallPath%\Common7\Tools\VsDevCmd.bat" (
   echo ERROR: Visual Studio developer command script was not found.
   exit /b 1
)

call "%vsInstallPath%\Common7\Tools\VsDevCmd.bat" -arch=%platform% -host_arch=x64
if errorlevel 1 exit /b 1

:compiler_ready
where cl >nul 2>nul
if errorlevel 1 (
   echo ERROR: cl.exe was not found after initializing Visual Studio.
   exit /b 1
)
where rc >nul 2>nul
if errorlevel 1 (
   echo ERROR: The Windows SDK resource compiler rc.exe was not found.
   exit /b 1
)

set warnFlags=-FC
set includeFlags=-I..\third_party\SDL2-2.0.8\include -I..\third_party\imgui\ -I..\third_party

if not exist build mkdir build
if errorlevel 1 exit /b 1
pushd build
if errorlevel 1 exit /b 1
cl /Zi ..\src\shadergen.cc %warnFlags%
if errorlevel 1 (
   popd
   exit /b 1
)
popd

taskkill /f /im milton.exe >nul 2>&1

build\shadergen.exe
if errorlevel 1 exit /b 1

pushd build
if errorlevel 1 exit /b 1

copy "..\milton_icon.ico" "milton_icon.ico"
if errorlevel 1 (
   popd
   exit /b 1
)
copy "..\third_party\Carlito.ttf" "Carlito.ttf"
if errorlevel 1 (
   popd
   exit /b 1
)
copy "..\third_party\Carlito.LICENSE" "Carlito.LICENSE"
if errorlevel 1 (
   popd
   exit /b 1
)
copy ..\Milton.rc Milton.rc
if errorlevel 1 (
   popd
   exit /b 1
)
rc Milton.rc
if errorlevel 1 (
   popd
   exit /b 1
)

set compiler_flags=/O2 /MTd /Zi %includeFlags% %warnFlags% /Femilton.exe /wd4217 /link ..\third_party\bin\%platform%\SDL2.lib OpenGL32.lib gdi32.lib shell32.lib comdlg32.lib ole32.lib oleAut32.lib winmm.lib advapi32.lib version.lib dxgi.lib pdh.lib psapi.lib

if /i "%~1"=="test" (
   cl ..\src\unity_tests.cc %compiler_flags /SUBSYSTEM:Console
) else (
   cl Milton.res ..\src\unity.cc %compiler_flags%
)
if errorlevel 1 (
   popd
   exit /b 1
)
:: ..\third_party\bin\%platform%\SDL2.lib
popd
exit /b 0
