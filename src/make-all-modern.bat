@REM @SET PATH=C:/MinGW/32/bin/;C:/MinGW/msys/bin/;

@REM make -f Makefile build ARCH=x86-32-abm         COMP=mingw VER=ddmmyy
@REM make -f Makefile build ARCH=x86-32-abm         COMP=mingw

@REM -----------------------------------------------------

@SET PATH=C:/MinGW/64/bin/;C:/MinGW/msys/bin/;

@REM make -f MakeFile build         ARCH=x86-64-abm COMP=mingw VER=ddmmyy

@REM make -f Makefile build         ARCH=x86-64-abm COMP=mingw
@REM make -f Makefile build         ARCH=x86-64-bm2 COMP=mingw

@REM make -f Makefile profile-build ARCH=x86-64-abm COMP=mingw
@REM make -f Makefile profile-build ARCH=x86-64-bm2 COMP=mingw

@REM make -f Makefile build         ARCH=x86-64-abm COMP=mingw
make -f Makefile build         ARCH=x86-64-bm2 COMP=mingw debug=no
@REM make -f Makefile profile-build ARCH=x86-64-abm COMP=mingw
@REM make -f Makefile profile-build ARCH=x86-64-bm2 COMP=mingw

@PAUSE
