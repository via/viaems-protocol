CLAGS=-O3 -flto -g
CFLAGS+= -I tinycbor/src
LDLIBS= -lusb-1.0 -L tinycbor/lib -l:libtinycbor.a

ALL: libviaems.a example

linked-viaems-c.o: viaems-c.o
	ld -r -o linked-viaems-c.o viaems-c.o tinycbor/lib/libtinycbor.a

libviaems.a: linked-viaems-c.o viaems-usb.o
	ar rcs libviaems.a linked-viaems-c.o viaems-usb.o

example: example.o viaems-c.o viaems-usb.o

clean:
	-rm example.o viaems-c.o viaems-usb.o example libviaems.a
