INCLUDE = $(WATCOM)/h

CC = wcl
LD = wlink
ALL = txf.exe

all:	$(ALL)

txf.exe: txf.c
	$(CC) -bt=dos $<

clean:
	rm -f $(ALL) *.o *.err
