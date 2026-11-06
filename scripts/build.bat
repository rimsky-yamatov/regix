@echo off
rem Build rgc, rgfmt, rgpm and run_tests with MSVC (run from a developer prompt).
if not exist build mkdir build
set CF=/nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS /Icommon /Irgc
cl %CF% /Fobuild\ /Febuild\rgc.exe rgc\*.c common\*.c || exit /b 1
cl %CF% /Fobuild\ /Febuild\rgfmt.exe rgfmt\main.c common\util.c || exit /b 1
cl %CF% /Fobuild\ /Febuild\rgpm.exe rgpm\main.c || exit /b 1
cl %CF% /Fobuild\ /Febuild\run_tests.exe tests\run_tests.c common\util.c || exit /b 1
