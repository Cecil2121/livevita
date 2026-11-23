@echo off
setlocal EnableExtensions
title LiveVita - build the VPK
cd /d "%~dp0"

echo.
echo  ===========================================
echo    LiveVita VPK builder
echo  ===========================================
echo.

if not exist "CMakeLists.txt" (
    echo  This file must stay inside the LiveVita folder, next to CMakeLists.txt.
    goto :end
)

rem ---- 1. WSL (Ubuntu inside Windows) is needed to run the Vita build tools
where wsl >nul 2>&1 || goto :no_wsl
wsl -e true >nul 2>&1 || goto :no_wsl

rem ---- 2. Twitch Client ID (remembered in tools\client_id.txt)
set "CID="
if exist "tools\client_id.txt" set /p CID=<"tools\client_id.txt"
if defined CID (
    echo  Using your saved Twitch Client ID: %CID%
    echo  ^(To change it, delete tools\client_id.txt and run this again.^)
) else (
    echo  Paste your Twitch Client ID from dev.twitch.tv/console
    echo  ^(right-click pastes^), or just press Enter to skip.
    set /p "CID=  Client ID: "
)
if defined CID (
    >"tools\client_id.txt" echo %CID%
)
echo.

rem ---- 3. Build inside WSL
rem (fix Windows line endings in case the script was edited or unzipped oddly)
wsl -e sed -i "s/\r$//" tools/build_vpk.sh
wsl -e bash tools/build_vpk.sh "%CID%"
if errorlevel 1 (
    echo.
    echo  The build failed. Scroll up to see the error.
    echo  You can send the error text to get help fixing it.
    goto :end
)

if exist "Twitch.vpk" (
    echo.
    echo  Success! Copy Twitch.vpk to your Vita and install it with VitaShell.
    explorer /select,"%~dp0Twitch.vpk"
)
goto :end

:no_wsl
echo  This builder needs WSL ^(Ubuntu running inside Windows^), which is free
echo  from Microsoft. It is not installed yet, or Ubuntu has not been set up.
echo.
choice /c YN /m "  Install it now? (Windows will ask for administrator permission)"
if errorlevel 2 goto :end
powershell -NoProfile -Command "Start-Process wsl -ArgumentList '--install','-d','Ubuntu' -Verb RunAs -Wait"
echo.
echo  Next steps:
echo    1. Restart your PC when the install finishes.
echo    2. Ubuntu opens by itself ^(or open "Ubuntu" from the Start menu^):
echo       choose a username and password and remember the password.
echo    3. Run Build-VPK.bat again.
goto :end

:end
echo.
pause
