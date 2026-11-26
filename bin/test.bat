@echo off
setlocal
if defined QTDIR set "PATH=%QTDIR%\bin;%PATH%"
cd /d "%~dp0.."
for /f "tokens=2 delims=:" %%C in ('chcp') do set /a steady_cp=%%C >nul
if "%steady_cp%"=="65001" chcp 437 >nul
call :test
set "steady_rc=%errorlevel%"
if "%steady_cp%"=="65001" chcp 65001 >nul
exit /b %steady_rc%

:test
if not exist build\test mkdir build\test
cd build
if exist .qmake.stash (findstr /b /c:"QMAKE_CXX.QMAKE_MSC_VER" .qmake.stash >nul || del .qmake.stash)
qmake ..\steady-cli.pro -o Makefile.cli "CONFIG-=debug_and_release" "CONFIG+=release" || exit /b 1
nmake /nologo /f Makefile.cli || exit /b 1
cd test
if exist .qmake.stash (findstr /b /c:"QMAKE_CXX.QMAKE_MSC_VER" .qmake.stash >nul || del .qmake.stash)
qmake ..\..\tests\test_steady.pro "CONFIG-=debug_and_release" "CONFIG+=release" || exit /b 1
nmake /nologo || exit /b 1
test_steady.exe || exit /b 1
cd ..\..
if not exist build\test-overlay mkdir build\test-overlay
cd build\test-overlay
if exist .qmake.stash (findstr /b /c:"QMAKE_CXX.QMAKE_MSC_VER" .qmake.stash >nul || del .qmake.stash)
qmake ..\..\tests\test_overlay.pro "CONFIG-=debug_and_release" "CONFIG+=release" || exit /b 1
nmake /nologo || exit /b 1
set "QT_QPA_PLATFORM=offscreen"
test_overlay.exe
