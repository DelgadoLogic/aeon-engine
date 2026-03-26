@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

set IDIR=C:\Users\Manuel A Delgado\Desktop\AeonBrowser\installer
set SRC=%IDIR%\src
set BIN=%IDIR%\bin
set OBJ=%IDIR%\obj

if not exist "%BIN%" mkdir "%BIN%"
if not exist "%OBJ%" mkdir "%OBJ%"

echo [1/3] Resource compile...
rc.exe /nologo /fo "%OBJ%\AeonInstaller.res" "%SRC%\AeonInstaller.rc"
if errorlevel 1 ( echo RC FAILED & goto done )

echo [2/3] C++ compile...
cl.exe /nologo /W3 /O2 /std:c++17 /EHsc /DUNICODE /D_UNICODE /DWIN32 /DNDEBUG /Fo"%OBJ%\AeonInstaller.obj" /c "%SRC%\AeonInstaller.cpp"
if errorlevel 1 ( echo CL FAILED & goto done )

echo [3/3] Link...
link.exe /nologo /SUBSYSTEM:WINDOWS /MACHINE:X64 /OUT:"%BIN%\AeonSetup.exe" "%OBJ%\AeonInstaller.obj" "%OBJ%\AeonInstaller.res" gdiplus.lib shell32.lib shlwapi.lib user32.lib gdi32.lib ole32.lib kernel32.lib advapi32.lib
if errorlevel 1 ( echo LINK FAILED & goto done )

echo.
echo =========================================
echo  BUILD SUCCESS: installer\bin\AeonSetup.exe
echo =========================================

REM Zero PE timestamp (metadata scrub)
powershell -NoProfile -Command "$f='%BIN%\AeonSetup.exe';$b=[IO.File]::ReadAllBytes($f);$o=[BitConverter]::ToInt32($b,0x3C)+8;[BitConverter]::GetBytes([uint32]0)|ForEach-Object{$i=0}{$b[$o+$i]=$_;$i++};[IO.File]::WriteAllBytes($f,$b);Write-Host 'PE timestamp zeroed.'"

:done
