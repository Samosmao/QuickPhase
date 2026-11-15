@echo off
setlocal
cd /d "%~dp0"
set CXX=C:\msys64\ucrt64\bin\g++.exe
set WINDRES=C:\msys64\ucrt64\bin\windres.exe

"%WINDRES%" app_res.rc -O coff -o app_res.o
if %errorlevel% neq 0 exit /b %errorlevel%

"%CXX%" -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static -static-libgcc -static-libstdc++ ^
  main.cpp storage.cpp clipboard_win.cpp popup.cpp settings_wnd.cpp ocr_snip.cpp updater.cpp app_res.o ^
  -o QuickPhrase.exe ^
  -lgdiplus -lgdi32 -luser32 -lshell32 -lcomctl32 -lcomdlg32 -lole32 -lshlwapi -lwinhttp
if %errorlevel% neq 0 (
    echo Build QuickPhrase.exe FAILED!
    exit /b %errorlevel%
)

"%WINDRES%" setup_res.rc -O coff -o setup_res.o
if %errorlevel% neq 0 exit /b %errorlevel%

"%CXX%" -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static -static-libgcc -static-libstdc++ ^
  setup.cpp setup_res.o ^
  -o QuickPhraseSetup.exe ^
  -lgdi32 -luser32 -lshell32 -lole32 -luuid
if %errorlevel% neq 0 (
    echo Build QuickPhraseSetup.exe FAILED!
    exit /b %errorlevel%
)

echo Build SUCCEEDED: QuickPhrase.exe and QuickPhraseSetup.exe (Publisher: Mine1918)
