@echo off
setlocal
cd /d "%~dp0\.."
if not exist test\build mkdir test\build

echo [1/2] protocol/format/battery tests
g++ -std=c++11 -Wall -Wextra -Iinclude -Isrc test\test_protocol.cpp src\protocol_format.cpp src\battery.cpp src\settings.cpp -o test\build\test_protocol.exe
if errorlevel 1 exit /b 1
test\build\test_protocol.exe
if errorlevel 1 exit /b 1

echo [2/2] communication parser tests
g++ -std=c++11 -Wall -Wextra -Itest\stubs -Iinclude -Isrc test\test_comm.cpp src\comm.cpp src\protocol_format.cpp src\settings.cpp -o test\build\test_comm.exe
if errorlevel 1 exit /b 1
test\build\test_comm.exe
if errorlevel 1 exit /b 1

echo.
echo All host tests passed.
