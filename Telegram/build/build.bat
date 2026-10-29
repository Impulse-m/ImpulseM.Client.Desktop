@echo OFF
setlocal enabledelayedexpansion
set "FullScriptPath=%~dp0"
set "FullExecPath=%cd%"

rem ImpulseM Windows release build (x64 only). Produces the Inno Setup installer
rem and the portable archive in out\Release\deploy\<major>\<version>, with
rem SHA256SUMS.txt beside them. Signing runs only when IMPULSEM_SIGN_PFX is set
rem (see sign_windows.bat); without it the artifacts are unsigned.

if "%VSCMD_ARG_HOST_ARCH%" neq "x64" (
  echo Bad environment. Make sure to run from 'x64 Native Tools Command Prompt for VS'.
  exit /b 1
)
if "%VSCMD_ARG_TGT_ARCH%" neq "x64" (
  echo Bad environment. Make sure to run from 'x64 Native Tools Command Prompt for VS'.
  exit /b 1
)

set "HomePath=%FullScriptPath%.."
set "SolutionPath=%HomePath%\..\out"
set "ReleasePath=%SolutionPath%\Release"
set "BinaryName=ImpulseM"
set "ModulesPath=modules\x64"

FOR /F "tokens=1,2* delims= " %%i in (%FullScriptPath%version) do set "%%i=%%j"

set "AppVersionStrFull=%AppVersionStr%"
if %AlphaVersion% neq 0 (
  echo Alpha builds are not supported by the ImpulseM release script.
  exit /b 1
)
if %BetaChannel% neq 0 (
  set "AppVersionStrFull=%AppVersionStr%.beta"
)

set "SetupBaseName=%BinaryName%-setup-x64-%AppVersionStrFull%"
set "SetupFile=%SetupBaseName%.exe"
set "PortableFile=%BinaryName%-portable-x64-%AppVersionStrFull%.zip"
set "DeployPath=%ReleasePath%\deploy\%AppVersionStrMajor%\%AppVersionStrFull%"

if exist "%DeployPath%\" (
  echo Deploy folder for version %AppVersionStrFull% already exists!
  exit /b 1
)

set "PATH=%PATH%;C:\Program Files (x86)\Inno Setup 6;C:\Program Files\7-Zip"

echo.
echo Building version %AppVersionStrFull% for Windows 64 bit..
echo.

call :build
set "ErrorCode=%errorlevel%"
cd "%FullExecPath%"
exit /b %ErrorCode%

:build
cd "%HomePath%"

call configure.bat -DTDESKTOP_API_ID=%IMPULSEM_API_ID% -DTDESKTOP_API_HASH=%IMPULSEM_API_HASH% -DDESKTOP_APP_ENABLE_LTO=ON -DDESKTOP_APP_DISABLE_AUTOUPDATE=ON -DDESKTOP_APP_DISABLE_CRASH_REPORTS=ON || goto error

cd "%SolutionPath%"
call cmake --build . --config Release --target Telegram || goto error

echo.
echo Version %AppVersionStrFull% build successful. Preparing..
echo.

cd "%ReleasePath%"

call "%FullScriptPath%sign_windows.bat" "%BinaryName%.exe" || goto error

iscc /dMyAppVersion=%AppVersionStrSmall% /dMyAppVersionZero=%AppVersionStr% /dMyAppVersionFull=%AppVersionStrFull% "/dReleasePath=%ReleasePath%" "/dMyBuildTarget=win64" "/dMyOutputBaseFilename=%SetupBaseName%" "%FullScriptPath%setup.iss" || goto error
if not exist "%SetupFile%" goto error

call "%FullScriptPath%sign_windows.bat" "%SetupFile%" || goto error

mkdir "%DeployPath%" || goto error

move /Y "%ReleasePath%\%SetupFile%" "%DeployPath%\" || goto error

set "StagePath=%DeployPath%\%BinaryName%"
mkdir "%StagePath%\%ModulesPath%\d3d" || goto error
copy /Y "%ReleasePath%\%BinaryName%.exe" "%StagePath%\" || goto error
copy /Y "%ReleasePath%\%ModulesPath%\d3d\d3dcompiler_47.dll" "%StagePath%\%ModulesPath%\d3d\" || goto error
xcopy "%ReleasePath%\%ModulesPath%\livekit" "%StagePath%\%ModulesPath%\livekit\" /E /I /Y || goto error
xcopy "%ReleasePath%\%ModulesPath%\xray" "%StagePath%\%ModulesPath%\xray\" /E /I /Y || goto error
mkdir "%StagePath%\impulse" || goto error
copy /Y "%ReleasePath%\impulse\tl-proto-232.json.gz" "%StagePath%\impulse\" || goto error

cd "%DeployPath%"
7z a -mx9 "%PortableFile%" "%BinaryName%" || goto error
7z t "%PortableFile%" || goto error
rmdir /S /Q "%StagePath%" || goto error

powershell -NoProfile -ExecutionPolicy Bypass -File "%FullScriptPath%write_checksums.ps1" -Directory "%DeployPath%" -Files "%SetupFile%,%PortableFile%" || goto error

echo.
echo Version %AppVersionStrFull% is ready: %DeployPath%
echo.

cd "%FullExecPath%"
exit /b

:error
(
  set ErrorCode=%errorlevel%
  if !ErrorCode! neq 0 (
    echo Error !ErrorCode!
  ) else (
    echo Error 666
    set ErrorCode=666
  )
  cd "%FullExecPath%"
  exit /b !ErrorCode!
)
