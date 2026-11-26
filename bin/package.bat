@echo off
setlocal EnableExtensions
if defined QTDIR set "PATH=%QTDIR%\bin;%PATH%"
cd /d "%~dp0.."
for /f "tokens=2 delims=:" %%C in ('chcp') do set /a steady_cp=%%C >nul
if "%steady_cp%"=="65001" chcp 437 >nul
call :package %*
set "steady_rc=%errorlevel%"
if defined work if exist "%work%" rmdir /s /q "%work%"
if "%steady_cp%"=="65001" chcp 65001 >nul
exit /b %steady_rc%

:package
set "root=%CD%"
set /p "version="<version.txt
if not defined version exit /b 1
set "archive=steady-%version%-windows-x64.zip"
set "work=%root%\dist\.package-%RANDOM%-%RANDOM%"
set "build=%work%\build"
set "stage=%work%\files"
set "licenses=%stage%\THIRD-PARTY-LICENSES"
mkdir "%build%" "%licenses%" || exit /b 1

pushd "%build%"
qmake "%root%\steady.pro" -o Makefile "CONFIG-=debug_and_release" "CONFIG-=debug" "CONFIG+=release" %* || (popd & exit /b 1)
nmake /nologo /f Makefile || (popd & exit /b 1)
qmake "%root%\steady-cli.pro" -o Makefile.cli "CONFIG-=debug_and_release" "CONFIG-=debug" "CONFIG+=release" %* || (popd & exit /b 1)
nmake /nologo /f Makefile.cli || (popd & exit /b 1)
popd

copy /y "%build%\steady.exe" "%stage%\" >nul || exit /b 1
copy /y "%build%\steady-cli.exe" "%stage%\" >nul || exit /b 1
copy /y LICENSE "%stage%\" >nul || exit /b 1
copy /y README.md "%stage%\" >nul || exit /b 1
copy /y packaging\THIRD-PARTY-NOTICES.md "%licenses%\README.md" >nul || exit /b 1
copy /y packaging\licenses\* "%licenses%\" >nul || exit /b 1
copy /y third_party\qrcodegen\LICENSE "%licenses%\qrcodegen-MIT.txt" >nul || exit /b 1

set "openssl_root=%STEADY_OPENSSL_ROOT%"
if not defined openssl_root if exist "C:\Program Files\OpenSSL-Win64\bin\openssl.exe" set "openssl_root=C:\Program Files\OpenSSL-Win64"
if not defined openssl_root if exist "C:\Program Files\Git\usr\bin\openssl.exe" set "openssl_root=C:\Program Files\Git\usr"
if not defined openssl_root (
  echo A redistributable OpenSSL 3 root is required. Set STEADY_OPENSSL_ROOT. 1>&2
  exit /b 1
)

windeployqt --release --compiler-runtime --openssl-root "%openssl_root%" --qmldir src "%stage%\steady.exe" "%stage%\steady-cli.exe" || exit /b 1
copy /y "%openssl_root%\bin\openssl.exe" "%stage%\" >nul || exit /b 1
copy /y "%openssl_root%\bin\libssl-3*.dll" "%stage%\" >nul || exit /b 1
copy /y "%openssl_root%\bin\libcrypto-3*.dll" "%stage%\" >nul || exit /b 1
if exist "%openssl_root%\license.txt" copy /y "%openssl_root%\license.txt" "%licenses%\OpenSSL-Windows-LICENSE.txt" >nul

rem windeployqt may omit an installed MSVC runtime. Copy the exact runtime used
rem by the active developer environment so the archive works on a clean host.
if defined VCToolsRedistDir (
  for %%F in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll) do (
    if not exist "%stage%\%%F" copy /y "%VCToolsRedistDir%x64\Microsoft.VC143.CRT\%%F" "%stage%\" >nul || exit /b 1
  )
)

for %%F in (vcruntime140.dll vcruntime140_1.dll msvcp140.dll openssl.exe libssl-3-x64.dll libcrypto-3-x64.dll) do (
  if not exist "%stage%\%%F" (
    echo Missing required runtime file %%F 1>&2
    exit /b 1
  )
)
if not exist "%stage%\tls\qopensslbackend.dll" (
  echo Missing Qt OpenSSL TLS backend 1>&2
  exit /b 1
)

powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $s=(Resolve-Path $env:stage).Path; Get-ChildItem -LiteralPath $s -Recurse -File | ForEach-Object { $_.FullName.Substring($s.Length + 1).Replace('\','/') } | Sort-Object | Set-Content -Encoding UTF8 (Join-Path $env:licenses 'BUNDLED-FILES.txt')" || exit /b 1
tar.exe -a -cf "%work%\%archive%" -C "%stage%" . || exit /b 1
move /y "%work%\%archive%" "dist\%archive%" >nul || exit /b 1
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $names=@(('steady-' + $env:version + '-linux-x86_64.tar.gz'),('steady-' + $env:version + '-macos-arm64.zip'),('steady-' + $env:version + '-windows-x64.zip')); $lines=foreach($name in $names) { $path=Join-Path (Join-Path $env:root 'dist') $name; if(Test-Path -LiteralPath $path) { (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() + '  ' + $name } }; $lines | Set-Content -Encoding ASCII (Join-Path $env:work 'SHA256SUMS.txt')" || exit /b 1
move /y "%work%\SHA256SUMS.txt" dist\SHA256SUMS.txt >nul || exit /b 1
echo Created dist\%archive%
exit /b 0
