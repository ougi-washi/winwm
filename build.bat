@echo off
setlocal
pushd "%~dp0" || exit /b 1
echo [1/4] Checking the Windows x64 toolchain
if not defined CC (
    where gcc >nul 2>nul && set "CC=gcc"
)
if not defined CC (
    where cc >nul 2>nul && set "CC=cc"
)
if not defined CC (
    echo Install MinGW-w64 GCC or set CC to an x64 Windows C compiler. 1>&2
    goto :fail
)
if not defined WINDRES set "WINDRES=windres"
if not exist build mkdir build || goto :fail
if not exist bin mkdir bin || goto :fail

echo [2/4] Building winwm and its recovery monitor
"%CC%" @build.rsp -o build\winwm.exe || goto :fail
"%CC%" @recovery.rsp -o build\winwm_recovery.exe || goto :fail

echo [3/4] Compiling the installer manifest
"%WINDRES%" -I src/install -i src/install/install.rc -O coff -o build/install_res.o || goto :fail

echo [4/4] Building winwm_install.exe
"%CC%" @install.rsp -o build\winwm_install.exe || goto :fail
move /y build\winwm.exe bin\winwm.exe >nul || goto :fail
move /y build\winwm_recovery.exe bin\winwm_recovery.exe >nul || goto :fail
move /y build\winwm_install.exe bin\winwm_install.exe >nul || goto :fail
echo Built winwm, recovery monitor, and installer in bin.
popd
exit /b 0

:fail
popd
exit /b 1
