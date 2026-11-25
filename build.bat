@echo off
setlocal
pushd "%~dp0" || exit /b 1
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
where mingw32-make >nul 2>nul || (
    echo Install MinGW-w64 Make ^(mingw32-make^) to build winwm. 1>&2
    goto :fail
)
set "winwm_jobs=%NUMBER_OF_PROCESSORS%"
if not defined winwm_jobs set "winwm_jobs=1"
mingw32-make -j%winwm_jobs% %* || goto :fail
popd
exit /b 0

:fail
popd
exit /b 1
