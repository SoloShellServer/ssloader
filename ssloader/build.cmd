@echo off
rem Builds ..\ssloader.dll (32-bit, like WoW.exe) with Visual Studio 2022.
rem /Brepro and no /DEBUG: the same source gives the same bytes and no build path ends up in the DLL.
setlocal
call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O1 /MT /W4 /GS /Brepro /LD ssloader.c /Fo:"%TEMP%\ssloader.obj" /Fe:..\ssloader.dll ^
   /link /DEF:ssloader.def /INCREMENTAL:NO /Brepro kernel32.lib || exit /b 1
del ..\ssloader.lib ..\ssloader.exp 2>nul
exit /b 0
