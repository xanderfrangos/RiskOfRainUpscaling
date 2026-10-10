@echo off
setlocal
set "BUILD_NO_PAUSE="
if /i "%~1"=="-NoPause" set "BUILD_NO_PAUSE=1"

rem Prefer PowerShell 7, but the build also supports Windows PowerShell 5.1.
set "BUILD_SHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" set "BUILD_SHELL=%ProgramFiles%\PowerShell\7\pwsh.exe"
for /f "delims=" %%P in ('where pwsh.exe 2^>nul') do set "BUILD_SHELL=%%P"
rem Windows PowerShell cannot load modules inherited from a PowerShell 7 terminal.
if /i "%BUILD_SHELL%"=="%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" set "PSModulePath=%USERPROFILE%\Documents\WindowsPowerShell\Modules;%ProgramFiles%\WindowsPowerShell\Modules;%SystemRoot%\System32\WindowsPowerShell\v1.0\Modules"

rem Keep all arguments, including -NoPause, for command-line builds.
"%BUILD_SHELL%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0build\Build.ps1" %*
set "BUILD_EXIT=%ERRORLEVEL%"
echo.
if "%BUILD_EXIT%"=="0" (
    echo Build complete. Package: "%~dp0build\package"
    echo Thunderstore package: "%~dp0Package"
) else (
    echo Build failed. See "%~dp0build\Build.log" for details.
)
if not defined BUILD_NO_PAUSE pause
exit /b %BUILD_EXIT%
