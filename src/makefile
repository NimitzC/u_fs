all:diskimg_init u_fs
diskimg_init:diskimg_init.c
	gcc diskimg_init.c -o diskimg_init
u_fs:u_fs.c
	gcc -Wall u_fs.c `pkg-config fuse3 --cflags --libs` -o u_fs
.PHONY: all
clean:
	rm -f u_fs diskimg_init