# Notes

I am currently using a 4MiB loopback device with a blocksize of 4K.

```
losetup -fP -b 4096 ~/md.img
```

# Commands to run

```
make; make test; make flan_fuse
mkfs.flexalloc -s 64 /dev/loop0
LD_LIBRARY_PATH=. FLAN_TEST_DEV=/dev/loop0 ./flan_test
modprobe fuse
LD_LIBRARY_PATH=. ./flan_fuse --dev_uri=/dev/loop0 --poolname=TEST --obj_size=8192 /mnt/
ls /mnt
umount /mnt
```
