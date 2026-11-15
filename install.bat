@echo off
setlocal
cd /d "%~dp0"

REM Use adb from ANDROID_HOME if set, otherwise expect adb on PATH.
if defined ANDROID_HOME (
    set "ADB=%ANDROID_HOME%\platform-tools\adb.exe"
) else if defined ANDROID_SDK_ROOT (
    set "ADB=%ANDROID_SDK_ROOT%\platform-tools\adb.exe"
) else (
    set "ADB=adb"
)

echo.
echo === Step 1: check device ===
"%ADB%" devices
echo.
"%ADB%" devices | findstr /r /c:"device$" >nul
if errorlevel 1 (
    echo [STOP] No authorized device found.
    echo Check: data cable, USB mode = Transfer Files, RSA prompt on phone.
    exit /b 1
)

echo === Step 2: wake the phone ===
"%ADB%" shell input keyevent 224 >nul
"%ADB%" shell svc power stayon usb >nul
echo.
echo [ACTION ON PHONE] Unlock the screen now, close the notification shade,
echo and keep the screen on. Some ROMs will show an install confirmation.
echo Tap Allow / Continue install when it appears.
echo.
pause

echo === Step 3: install APK ===
if exist "android\app\build\outputs\apk\debug\app-debug.apk" (
    set "APK=android\app\build\outputs\apk\debug\app-debug.apk"
) else (
    set "APK=android\app\build\outputs\apk\release\app-release.apk"
)
echo Installing: %APK%
"%ADB%" install -r "%APK%"
if errorlevel 1 (
    echo [FAILED] The install was blocked or the APK is missing.
    echo  - Build it first: gradlew.bat assembleDebug
    echo  - Unlock the phone and tap Allow when the prompt appears.
    echo  - A signature mismatch means you must uninstall the old version first.
    exit /b 1
)

echo.
echo [OK] ClassMate installed. Open the app icon on your phone.
echo Then open Settings - Download recognition model before recording.
echo.
