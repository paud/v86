@echo off
REM ============================================================
REM  v86gl DLL installer for Windows 9x / XP guests in v86
REM  Copies the proxy DLLs from the CD to the system directory.
REM
REM  This batch file sits on the v86gl_install CD ISO and is
REM  intended to be run from the guest after mounting the CD.
REM ============================================================

setlocal

set SRC=%~dp0
echo Source: %SRC%
echo.

REM ---- Detect Windows version ----
REM  Win9x:  %OS% is not defined (or "Windows_95" on 95)
REM  NT/2K/XP: %OS% == "Windows_NT"
if "%OS%"=="Windows_NT" goto nt

REM ============================================================
REM  Windows 9x / ME
REM ============================================================
echo Detected Windows 9x/ME
set DST=%WINDIR%\SYSTEM
if not exist "%WINDIR%\SYSTEM\" goto no_win9x_system
echo Destination: %DST%
echo.

if exist "%SRC%OPENGL32.DLL" goto have_opengl_9x
echo WARNING: OPENGL32.DLL not found on CD - skipping OpenGL proxy.
goto ddraw_9x
:have_opengl_9x
copy /Y "%SRC%OPENGL32.DLL" "%DST%\OPENGL32.DLL" >nul
if errorlevel 1 goto copy_fail
echo Installed: OPENGL32.DLL -> %DST%\OPENGL32.DLL

:ddraw_9x
if exist "%SRC%DDRAW.DLL" goto have_ddraw_9x
echo WARNING: DDRAW.DLL not found on CD - skipping DirectDraw proxy.
goto done_9x
:have_ddraw_9x
copy /Y "%SRC%DDRAW.DLL" "%DST%\DDRAW.DLL" >nul
if errorlevel 1 goto copy_fail
echo Installed: DDRAW.DLL -> %DST%\DDRAW.DLL

:done_9x
echo.
echo Windows 9x installation complete.
echo Reboot the guest for the new DLLs to take effect.
goto end

:no_win9x_system
echo ERROR: Cannot find %WINDIR%\SYSTEM\ directory.
echo        Is this actually Windows 9x/ME?
goto end

REM ============================================================
REM  Windows NT / 2000 / XP
REM ============================================================
:nt
echo Detected Windows NT/2000/XP
set DST=%SystemRoot%\SYSTEM32
if not exist "%SystemRoot%\SYSTEM32\" goto no_nt_system32
echo Destination: %DST%
echo.

REM --- Backup originals if not already backed up ---
if exist "%DST%\opengl32.dll.v86bak" goto have_opengl_bak
if exist "%DST%\opengl32.dll" copy /Y "%DST%\opengl32.dll" "%DST%\opengl32.dll.v86bak" >nul
:have_opengl_bak

if exist "%DST%\ddraw.dll.v86bak" goto have_ddraw_bak
if exist "%DST%\ddraw.dll" copy /Y "%DST%\ddraw.dll" "%DST%\ddraw.dll.v86bak" >nul
:have_ddraw_bak

REM --- Install proxies ---
if exist "%SRC%OPENGL32.DLL" goto have_opengl_nt
echo WARNING: OPENGL32.DLL not found on CD - skipping OpenGL proxy.
goto ddraw_nt
:have_opengl_nt
copy /Y "%SRC%OPENGL32.DLL" "%DST%\opengl32.dll" >nul
if errorlevel 1 goto copy_fail
echo Installed: OPENGL32.DLL -> %DST%\opengl32.dll

:ddraw_nt
if exist "%SRC%DDRAW.DLL" goto have_ddraw_nt
echo WARNING: DDRAW.DLL not found on CD - skipping DirectDraw proxy.
goto done_nt
:have_ddraw_nt
copy /Y "%SRC%DDRAW.DLL" "%DST%\ddraw.dll" >nul
if errorlevel 1 goto copy_fail
echo Installed: DDRAW.DLL -> %DST%\ddraw.dll

:done_nt
echo.
echo Windows NT/2000/XP installation complete.
echo Originals backed up as *.v86bak in %DST%.
echo Reboot the guest for the new DLLs to take effect.
echo.
echo To restore the originals later:
echo   copy /Y "%DST%\opengl32.dll.v86bak" "%DST%\opengl32.dll"
echo   copy /Y "%DST%\ddraw.dll.v86bak" "%DST%\ddraw.dll"
goto end

:no_nt_system32
echo ERROR: Cannot find %SystemRoot%\SYSTEM32\ directory.
goto end

:copy_fail
echo.
echo ERROR: Failed to copy a file. Make sure you have write access
echo        to the system directory, and that no program is using
echo        the target DLL (run in Safe Mode if necessary).
goto end

:end
endlocal
