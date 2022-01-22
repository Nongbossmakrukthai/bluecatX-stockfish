path=C:\Msys64\mingw64\bin;C:\msys64\msys\msys\1.0\bin;C:\Msys64\bin;
title make clean
if exist msgbinCHK.vbs del msgbinCHK.vbs
if exist msgMKclean.vbs del msgMKclean.vbs
if exist msgtitlesuccess.vbs del msgtitlesuccess.vbs
make clean
(echo MsgBox "Make Clean (.o) Successfully!", 262192, "Make Clean")> msgMKclean.vbs
start msgMKclean.vbs
pause
exit