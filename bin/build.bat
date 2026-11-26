@echo off
setlocal
if defined QTDIR set "PATH=%QTDIR%\bin;%PATH%"
cd /d "%~dp0.."
for /f "tokens=2 delims=:" %%C in ('chcp') do set /a steady_cp=%%C >nul
if "%steady_cp%"=="65001" chcp 437 >nul
call :build %*
set "steady_rc=%errorlevel%"
if "%steady_cp%"=="65001" chcp 65001 >nul
exit /b %steady_rc%

:build
if not exist build mkdir build
if exist build\.qmake.stash (findstr /b /c:"QMAKE_CXX.QMAKE_MSC_VER" build\.qmake.stash >nul || del build\.qmake.stash)
qmake steady.pro -o build\Makefile "CONFIG-=debug_and_release" "CONFIG+=release" %* || exit /b 1
cd build
nmake /nologo || exit /b 1
qmake ..\steady-cli.pro -o Makefile.cli "CONFIG-=debug_and_release" "CONFIG+=release" %* || exit /b 1
nmake /nologo /f Makefile.cli
