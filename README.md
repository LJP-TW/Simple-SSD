# Simple SSD
NYCU Operating System Capstone, Spring 2022

Final Project: Simple SSD

# Build
Set macro `NAND_LOCATION` in `ssd_fuse_header.h` to some path (e.g. The directory of this repository). `ssd_fuse` will create some files in this path to emulate NAND.

Then build `ssd_fuse` and `ssd_fuse_dut`:
```
./make_ssd
```

# Run
Termianl 1:
```
mkdir /tmp/ssd
./ssd_fuse -d /tmp/ssd
```

Terminal 2:
```
sh test.sh test1
```
