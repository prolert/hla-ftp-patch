@echo off
setlocal DisableDelayedExpansion
set "HLA_FTP_SCRIPT=%~dp0uninstall.ps1"
set "HLA_FTP_GAME_PATH="
set "HLA_FTP_HAS_GAME="
:parse
if "%~1"=="" goto empty
if /i "%~1"=="-GamePath" goto game
set "HLA_FTP_ARGUMENT=%~1"
if "%HLA_FTP_ARGUMENT:~0,1%"=="-" goto invalid
if defined HLA_FTP_HAS_GAME goto invalid
set "HLA_FTP_GAME_PATH=%~1"
set "HLA_FTP_HAS_GAME=1"
shift
goto parse
:game
if defined HLA_FTP_HAS_GAME goto invalid
if "%~2"=="" goto missing
if /i "%~2"=="-GamePath" goto missing
if /i "%~2"=="-PackagePath" goto missing
set "HLA_FTP_GAME_PATH=%~2"
set "HLA_FTP_HAS_GAME=1"
shift
shift
goto parse
:empty
if "%1"=="" goto run
goto missing
:run
rem Pass paths through the environment so trailing backslashes are not parsed as escaped quotes.
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $parameters=@{}; if($env:HLA_FTP_HAS_GAME){$parameters.GamePath=$env:HLA_FTP_GAME_PATH}; & $env:HLA_FTP_SCRIPT @parameters; exit $LASTEXITCODE"
set "result=%errorlevel%"
pause
exit /b %result%
:missing
echo Missing path value. Use -GamePath path. 1>&2
exit /b 1
:invalid
echo Unknown or duplicate option. Use -GamePath path or one positional game path. 1>&2
exit /b 1
