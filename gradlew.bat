@echo off
setlocal
set "APP_HOME=%~dp0"
set "WRAPPER_JAR=%APP_HOME%gradle\wrapper\gradle-wrapper.jar"

REM Fallback for shells that do not see the user environment variables yet.
REM On other machines set JAVA_HOME normally and this block is skipped.
if not defined JAVA_HOME (
    if exist "E:\ClassMateDev\jdk\17\bin\java.exe" set "JAVA_HOME=E:\ClassMateDev\jdk\17"
)
if not defined ANDROID_HOME (
    if exist "E:\ClassMateDev\android-sdk" set "ANDROID_HOME=E:\ClassMateDev\android-sdk"
)
if not defined GRADLE_USER_HOME (
    if exist "E:\ClassMateDev" set "GRADLE_USER_HOME=E:\ClassMateDev\gradle-home"
)

if not exist "%WRAPPER_JAR%" (
    echo [ERROR] gradle-wrapper.jar is missing.
    echo Please re-copy the project folder, or see the M0 guide.
    exit /b 1
)

if defined JAVA_HOME (
    set "JAVA_EXE=%JAVA_HOME%\bin\java.exe"
) else (
    set "JAVA_EXE=java"
)

"%JAVA_EXE%" -version >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Java not found. Install JDK 17 and set JAVA_HOME.
    echo See the M0 guide markdown file in this folder step 2.
    exit /b 1
)

"%JAVA_EXE%" -Djavax.net.ssl.trustStoreType=WINDOWS-ROOT -classpath "%WRAPPER_JAR%" org.gradle.wrapper.GradleWrapperMain %*
exit /b %ERRORLEVEL%
