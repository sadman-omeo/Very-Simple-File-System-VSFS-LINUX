Here’s a clean **README.md** you can drop in the zip (updated to the new filenames `mkfs_builder.c` and `mkfs_adder.c`).

---

# MiniVSFS — Quick Start

Two C programs:

* **`mkfs_builder`**: creates a valid MiniVSFS disk image (`.img`) with just the root directory (`/`).
* **`mkfs_adder`**: takes an existing MiniVSFS image and a real host file, adds that file into `/`, and writes a **new** image.

---

## Files in this folder

* `mkfs_builder.c` — builder tool
* `mkfs_adder.c` — adder tool
* *(optional for debugging)* `minivsfs_ls.c` — tiny read-only lister to print `/`

---

## Requirements (Linux)

* `gcc` (C17 OK)
* `xxd`, `hexdump`, `dd` (usually installed)

---

## Build

```bash
gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c   -o mkfs_adder
# optional helper
# gcc -O2 -std=c17 -Wall -Wextra minivsfs_ls.c -o minivsfs_ls
```

**Optional Makefile**

```make
CC=gcc
CFLAGS=-O2 -std=c17 -Wall -Wextra
all: mkfs_builder mkfs_adder
mkfs_builder: mkfs_builder.c
	$(CC) $(CFLAGS) $< -o $@
mkfs_adder: mkfs_adder.c
	$(CC) $(CFLAGS) $< -o $@
clean:
	rm -f mkfs_builder mkfs_adder
```

---

## Usage

### 1) Create a filesystem image

```bash
./mkfs_builder --image fs.img --size-kib 4096 --inodes 256
```

* `--size-kib`: 180..4096 (multiple of 4)
* `--inodes`: 128..512

This writes `fs.img` with:

* Block 0: superblock (magic `MVSF`), checksum set
* Block 1: inode bitmap
* Block 2: data bitmap
* Blocks 3..N: inode table
* Remaining blocks: data region
* Root inode (#1) with `.` and `..` in its first data block

### 2) Add a real file to `/` and produce a new image

```bash
# the file must already exist on your host filesystem
./mkfs_adder --input fs.img --output fs2.img --file file_13.txt
```

* First-fit allocation for a free **inode** and **data blocks**
* If root’s first block is full, it **extends** root with another block
* Max file size: **49,152 bytes** (12 direct pointers × 4096)

---

## Typical workflow (copy-paste)

```bash
# 1) Build tools
gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder
gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c   -o mkfs_adder

# 2) Make a 4 MiB image with 256 inodes
./mkfs_builder --image fs.img --size-kib 4096 --inodes 256

# 3) Add an existing text file
echo "hello MiniVSFS" > file_13.txt
./mkfs_adder --input fs.img --output fs2.img --file file_13.txt
```

---

## Quick validations

### Sizes

```bash
ls -lh fs.img fs2.img
# both should be same size (e.g., 4.0M if size-kib=4096)
```

### Superblock magic (block 0)

```bash
xxd -g 1 -l 4 -s 0 fs2.img
# Expect: 46 53 56 4d  → “MVSF”
```

### Bitmaps (block 1 = inode, block 2 = data)

```bash
xxd -g 1 -l 32 -s $((4096*1)) fs2.img   # inode bitmap
xxd -g 1 -l 32 -s $((4096*2)) fs2.img   # data bitmap
# After adding one file, both often start with 03 (root + one file)
```

### (Optional) List `/` entries

```bash
# if you compiled the helper
./minivsfs_ls fs.img
./minivsfs_ls fs2.img
# fs.img: shows '.' and '..'
# fs2.img: shows '.', '..', and your file name
```

---

## Constraints & details (spec highlights)

* Block size = **4096 B**; inode size = **128 B**
* **12 direct** pointers per inode (no indirects)
* Only the **root directory** is supported
* Inodes are **1-indexed** (root is inode **1**)
* Checksums:

  * Superblock: CRC32 stored in its last 4 bytes
  * Inode: CRC32 stored in last 8 bytes (low 4 bytes carry the CRC)
  * Dirent (64B): XOR of bytes 0..62

---

## Troubleshooting

* **“Not a MiniVSFS image”** → Rebuild with this `mkfs_builder`; don’t mix with old formats.
* **“File too large for 12 direct blocks”** → Keep files ≤ 49,152 bytes.
* **Dir entry missing** → Ensure the host file exists; check you used a new `--output`.
* **No free blocks** → Create a larger image (`--size-kib`) and try again.

---

That’s it—compile, create `fs.img`, add files with `mkfs_adder`, and inspect with the quick checks above.
