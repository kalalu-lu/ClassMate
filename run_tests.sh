#!/usr/bin/env sh
set -e
cd "$(dirname "$0")"
mkdir -p build

SQLITE_DEFS="-DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_DQS=0"
CFLAGS="-std=c99 -Wall -Wextra -pedantic -I core/include -I core/src -I third_party/sqlite"
CORESRC="core/src/core.c core/src/wav_writer.c core/src/asr_chunker.c core/src/storage.c core/src/note.c core/src/note_llm.c core/src/tokenizer.c"

gcc -std=c99 -O2 $SQLITE_DEFS -c third_party/sqlite/sqlite3.c -o build/sqlite3.o

gcc $CFLAGS $CORESRC core/tests/test_wav.c build/sqlite3.o -o build/test_wav
gcc $CFLAGS $CORESRC core/tests/test_asr_chunker.c build/sqlite3.o -o build/test_asr_chunker
gcc $CFLAGS $CORESRC core/tests/test_storage.c build/sqlite3.o -o build/test_storage
gcc $CFLAGS $CORESRC core/tests/test_note_llm.c build/sqlite3.o -o build/test_note_llm

./build/test_wav
./build/test_asr_chunker
./build/test_storage
./build/test_note_llm
