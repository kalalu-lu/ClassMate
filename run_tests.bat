@echo off
setlocal
cd /d "%~dp0"

echo ClassMate M2 core self-test (core + storage + sqlite)
if not exist build mkdir build

set CFLAGS=-std=c99 -Wall -Wextra -pedantic -D__USE_MINGW_ANSI_STDIO=1 -I core/include -I core/src -I third_party/sqlite
set CORESRC=core\src\core.c core\src\wav_writer.c core\src\asr_chunker.c core\src\storage.c core\src\note.c core\src\note_llm.c core\src\tokenizer.c

echo [1/5] building sqlite3.o ...
gcc -std=c99 -O2 -D__USE_MINGW_ANSI_STDIO=1 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_DQS=0 -c third_party\sqlite\sqlite3.c -o build\sqlite3.o
if errorlevel 1 (
    echo [ERROR] gcc compile failed for sqlite3
    exit /b 1
)

echo [2/5] building test_wav ...
gcc %CFLAGS% %CORESRC% core\tests\test_wav.c build\sqlite3.o -o build\test_wav.exe
if errorlevel 1 (
    echo [ERROR] gcc compile failed for test_wav
    exit /b 1
)

echo [3/5] building test_asr_chunker ...
gcc %CFLAGS% %CORESRC% core\tests\test_asr_chunker.c build\sqlite3.o -o build\test_asr_chunker.exe
if errorlevel 1 (
    echo [ERROR] gcc compile failed for test_asr_chunker
    exit /b 1
)

echo [4/5] building test_storage ...
gcc %CFLAGS% %CORESRC% core\tests\test_storage.c build\sqlite3.o -o build\test_storage.exe
if errorlevel 1 (
    echo [ERROR] gcc compile failed for test_storage
    exit /b 1
)

echo [5/5] building test_note_llm ...
gcc %CFLAGS% %CORESRC% core\tests\test_note_llm.c build\sqlite3.o -o build\test_note_llm.exe
if errorlevel 1 (
    echo [ERROR] gcc compile failed for test_note_llm
    exit /b 1
)

build\test_wav.exe
if errorlevel 1 exit /b 1

build\test_asr_chunker.exe
if errorlevel 1 exit /b 1

build\test_storage.exe
if errorlevel 1 exit /b 1

build\test_note_llm.exe
exit /b %ERRORLEVEL%
