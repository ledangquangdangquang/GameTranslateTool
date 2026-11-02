@echo off
rem Build UnityTL (can MinGW: scoop install mingw, va cmake: scoop install cmake)
setlocal
set PATH=%USERPROFILE%\scoop\apps\mingw\current\bin;%PATH%
cmake -S "%~dp0." -B "%~dp0build\cm" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release || goto :err
cmake --build "%~dp0build\cm" -j 8 || goto :err
if not exist "%~dp0dist" mkdir "%~dp0dist"
copy /y "%~dp0build\cm\unitytl.exe" "%~dp0dist\" >nul
copy /y "%~dp0build\cm\unitytl-cli.exe" "%~dp0dist\" >nul
echo.
echo Xong: %~dp0dist\unitytl.exe
exit /b 0
:err
echo BUILD LOI
exit /b 1
