#--------------------------------
# finddupe makefile for Win32
#--------------------------------

CC=cl
CFLAGS=/O2 /Qpar /EHsc /utf-8 /nologo

objects = finddupe.obj myglob.obj

finddupe.exe: $(objects)
    $(CC) $(CFLAGS) /Fe: finddupe.exe $(objects)

finddupe.obj: finddupe.c
    $(CC) $(CFLAGS) /c finddupe.c

myglob.obj: myglob.c
    $(CC) $(CFLAGS) /c myglob.c

.PHONY: clean
clean:
	del finddupe.exe $(objects)