CFLAGS = -O2 -Wall
LDLIBS = -lserialport

all: nvtispflash

clean:
	rm -f nvtispflash *.o
