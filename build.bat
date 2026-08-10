@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

title Phantom Client - build

echo.
echo  ============================================
echo   Phantom Client   build
echo  ============================================
echo.

rem ---------------------------------------------------------------
rem  build.bat            normal build
rem  build.bat clean      wipe build\ first
rem  build.bat debug      Debug instead of Release
rem ---------------------------------------------------------------
set CONFIG=Release
set DOCLEAN=0

for %%A in (%*) do (
    if /i "%%A"=="clean" set DOCLEAN=1
    if /i "%%A"=="debug" set CONFIG=Debug
)

rem ---------------------------------------------------------------
rem  OneDrive breaks the build.
rem  It keeps files in _deps open for syncing, so CMake cannot
rem  delete the ImGui clone when it re-runs and the configure step
rem  dies with "Failed to remove directory".
rem ---------------------------------------------------------------
echo %CD% | find /i "OneDrive" >nul
if not errorlevel 1 (
    echo  [!] This folder is inside OneDrive.
    echo      OneDrive locks files while syncing, which makes CMake
    echo      fail when it tries to refresh its dependencies.
    echo.
    echo      Move the project somewhere outside OneDrive, for example:
    echo        robocopy "%CD%" "C:\dev\phantom-client" /E /XD build
    echo.
    choice /c YN /m "  Try to build here anyway"
    if errorlevel 2 exit /b 1
    echo.
)

rem ---------------------------------------------------------------
rem  Locate a JDK. Only the headers matter: jni.h and jvmti.h.
rem ---------------------------------------------------------------
if not "%JDK_PATH%"=="" goto :have_jdk

if exist "%JAVA_HOME%\include\jni.h" (
    set "JDK_PATH=%JAVA_HOME%"
    goto :have_jdk
)

for /d %%D in ("C:\Program Files\Java\jdk*") do (
    if exist "%%D\include\jni.h" set "JDK_PATH=%%D"
)
for /d %%D in ("C:\Program Files\Eclipse Adoptium\jdk*") do (
    if exist "%%D\include\jni.h" set "JDK_PATH=%%D"
)
for /d %%D in ("C:\Program Files\Zulu\zulu*") do (
    if exist "%%D\include\jni.h" set "JDK_PATH=%%D"
)
for /d %%D in ("C:\Program Files\Amazon Corretto\jdk*") do (
    if exist "%%D\include\jni.h" set "JDK_PATH=%%D"
)

:have_jdk
if "%JDK_PATH%"=="" (
    echo  [X] No JDK found.
    echo.
    echo      A JRE is not enough: the build needs jni.h and jvmti.h,
    echo      which only ship with a JDK.
    echo.
    echo      Install JDK 8, then either set JAVA_HOME or run:
    echo        set JDK_PATH=C:\Path\To\jdk
    echo        build.bat
    echo.
    pause
    exit /b 1
)

if not exist "%JDK_PATH%\include\jni.h" (
    echo  [X] jni.h not found under:
    echo      %JDK_PATH%\include
    echo      That path is probably a JRE rather than a JDK.
    echo.
    pause
    exit /b 1
)

set "JDK_CMAKE=%JDK_PATH:\=/%"

echo  JDK      : %JDK_PATH%
echo  Config   : %CONFIG%
echo.

rem ---------------------------------------------------------------
rem  Clean
rem ---------------------------------------------------------------
if "%DOCLEAN%"=="1" (
    if exist build (
        echo  Removing build\ ...
        rmdir /s /q build 2>nul
        if exist build (
            echo  [X] Could not delete build\.
            echo      Something is holding it open. Close Visual Studio
            echo      and any Explorer window sitting in that folder.
            pause
            exit /b 1
        )
    )
    echo.
)

if not exist build mkdir build

rem ---------------------------------------------------------------
rem  Configure
rem  -A x64 is not optional: a 32-bit DLL will not load into the
rem  64-bit javaw.exe that Lunar runs in.
rem ---------------------------------------------------------------
echo  [1/2] Configuring ...
echo.
cmake -S . -B build -A x64 -DJDK_PATH="%JDK_CMAKE%"
if errorlevel 1 goto :configure_failed

echo.
echo  [2/2] Compiling ...
echo.
cmake --build build --config %CONFIG% --parallel
if errorlevel 1 goto :build_failed

set "OUT=build\%CONFIG%\PhantomClient.dll"
if not exist "%OUT%" (
    echo.
    echo  [X] Build reported success but the DLL is missing:
    echo      %OUT%
    pause
    exit /b 1
)

echo.
echo  ============================================
echo   Done
echo  ============================================
echo.
echo   %CD%\%OUT%
echo.
echo   Inject into javaw.exe, not the Lunar launcher.
echo   INSERT opens the menu, DELETE ejects.
echo.

choice /c YN /m "  Open the output folder"
if not errorlevel 2 explorer "%CD%\build\%CONFIG%"
exit /b 0

rem ---------------------------------------------------------------
:configure_failed
echo.
echo  [X] Configure failed.
echo.
echo      "Failed to remove directory ... imgui-src"
echo        OneDrive or an open file is locking _deps.
echo        Move the project out of OneDrive, then: build.bat clean
echo.
echo      "Could not resolve host: github.com"
echo        CMake downloads MinHook and ImGui on the first run.
echo.
pause
exit /b 1

:build_failed
echo.
echo  [X] Compile failed. The first error above is the real one;
echo      everything after it is usually fallout.
echo.
pause
exit /b 1
