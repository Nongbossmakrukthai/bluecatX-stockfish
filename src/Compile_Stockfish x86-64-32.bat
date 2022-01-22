path=C:\Msys64\mingw64\bin;C:\msys64\msys\msys\1.0\bin;C:\Msys64\bin;
title x86-64
if exist bin del bin
if not exist bin mkdir bin
if exist bin (echo MsgBox "Bin Folder Created", 262192, "Stockfish Compiler")> msgbinCHK.vbs
start msgbinCHK.vbs
make clean
make profile-build ARCH=x86-64 COMP=mingw
strip stockfish.exe
mv stockfish.exe bin\stockfish64.exe
title x86-32
make clean
make profile-build ARCH=x86-32 COMP=mingw
strip stockfish.exe
mv stockfish.exe bin\stockfish32.exe
(echo MsgBox "Compile Successfully", 262192, "Stockfish Compiler")> msgtitlesuccess.vbs
start msgtitlesuccess.vbs
pause
exit