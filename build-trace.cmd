@echo off
setlocal
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`%VSWHERE% -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -S . -B build-trace -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DRT_TRACE_CALLS=ON || exit /b 1
cmake --build build-trace %* || exit /b 1
