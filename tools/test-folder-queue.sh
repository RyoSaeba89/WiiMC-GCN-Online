#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:$PATH"
cd "$(dirname "$0")/.."
mkdir -p tests/out/tmp
export TMPDIR="$PWD/tests/out/tmp" TMP="$PWD/tests/out/tmp" TEMP="$PWD/tests/out/tmp"

# Compile the production auto-advance code, not a copy of it: the folder queue
# and FindNextFile() straight out of wiimc.cpp, and the path/extension helpers
# straight out of fileop.cpp.
awk '{ sub(/\r$/, "") }
     /^#define FOLDER_QUEUE_MAX/ { copy=1 }
     copy { print }
     copy && /^}$/ && seen { exit }
     /^extern "C" bool FindNextFile/ { seen=1 }' source/wiimc.cpp \
     > tests/out/folder_queue_wiimc.inc

awk '{ sub(/\r$/, "") }
     /^void GetFullPath\(BROWSERENTRY \*entry, char \*path\)$/ { copy=1 }
     /^void GetExt\(char \*file, char \*ext\)$/ { copy=1 }
     /^bool IsAudioExt\(char \*ext\)$/ { copy=1 }
     copy { print }
     copy && /^}$/ { copy=0; print "" }' source/fileop.cpp \
     > tests/out/folder_queue_fileop.inc

awk '{ sub(/\r$/, "") }
     /^const char validAudioExtensions/ { copy=1 }
     copy { print }
     copy && /^};$/ { exit }' source/settings.h \
     > tests/out/folder_queue_extensions.inc

for f in folder_queue_wiimc folder_queue_fileop folder_queue_extensions; do
    test -s "tests/out/$f.inc" || { echo "empty extraction: $f" >&2; exit 1; }
done
grep -q "FolderQueueAdvance" tests/out/folder_queue_wiimc.inc
grep -q "extern \"C\" bool FindNextFile" tests/out/folder_queue_wiimc.inc
grep -q "IsAudioExt" tests/out/folder_queue_fileop.inc

g++ -std=gnu++11 -O2 -g -Wall -Wextra -Wno-unused-function \
    tests/test_folder_queue.cpp -o tests/out/test_folder_queue.exe
timeout 30s tests/out/test_folder_queue.exe
