@echo OFF
setlocal

rem Authenticode signing step for ImpulseM Windows release files.
rem Usage: sign_windows.bat <file>
rem
rem Signing runs only when IMPULSEM_SIGN_PFX is set to the path of the decoded
rem .pfx certificate. Without it the file stays unsigned and the script succeeds,
rem so unsigned builds keep working.
rem
rem Optional environment:
rem   IMPULSEM_SIGN_PFX_PASSWORD      password of the .pfx certificate
rem   IMPULSEM_SIGN_TIMESTAMP_URL     RFC 3161 timestamp server

if "%~1" equ "" (
  echo Usage: sign_windows.bat ^<file^>
  exit /b 1
)

if "%IMPULSEM_SIGN_PFX%" equ "" (
  echo Signing secret is not set, leaving unsigned: %~1
  exit /b 0
)

if not exist "%IMPULSEM_SIGN_PFX%" (
  echo Signing certificate not found: %IMPULSEM_SIGN_PFX%
  exit /b 1
)

set "SignTool=%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
if not exist "%SignTool%" (
  set "SignTool=signtool.exe"
)

if "%IMPULSEM_SIGN_TIMESTAMP_URL%" equ "" (
  set "IMPULSEM_SIGN_TIMESTAMP_URL=http://timestamp.digicert.com"
)

echo Signing %~1
if defined IMPULSEM_SIGN_PFX_PASSWORD (
  "%SignTool%" sign /fd sha256 /tr "%IMPULSEM_SIGN_TIMESTAMP_URL%" /td sha256 /f "%IMPULSEM_SIGN_PFX%" /p "%IMPULSEM_SIGN_PFX_PASSWORD%" "%~1" || goto fail
) else (
  "%SignTool%" sign /fd sha256 /tr "%IMPULSEM_SIGN_TIMESTAMP_URL%" /td sha256 /f "%IMPULSEM_SIGN_PFX%" "%~1" || goto fail
)

"%SignTool%" verify /pa /q "%~1" || goto fail

echo Signed: %~1
exit /b 0

:fail
echo Signing failed: %~1
exit /b 1
