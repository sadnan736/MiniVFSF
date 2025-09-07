# MiniVSFS — A Tiny, Teachable File System

**MiniVSFS** is a small, inode-based file system you build and manipulate entirely from user-space. It ships with two CLI tools:

* `mkfs_builder` — **formats** a byte-exact MiniVSFS disk image
* `mkfs_adder` — **adds a host file** into the image’s **root** directory (`/`)

This project is great for learning real file-system layout (superblock, bitmaps, inodes, data blocks) without the heavy machinery.

---

## ✨ Highlights

* **Block size:** 4096 bytes
* **Inode size:** 128 bytes
* **Only root directory (`/`)** is supported
* **No indirect pointers** (12 direct data pointers per inode)
* **One block each** for inode bitmap and data bitmap
* **Byte-exact** image layout for deterministic testing

---

## 🧩 On-Disk Layout (not to scale)

```
[0] Superblock
[1] Inode Bitmap
[2] Data  Bitmap
[3..] Inode Table
[..]  Data Region  (files + directory entries)
```

* **Superblock**: counts, sizes, absolute block starts, timestamps, checksum
* **Bitmaps**: track free/used inodes and data blocks
* **Inode table**: file metadata (mode, links, size, times, direct\[12], crc)
* **Data region**: raw file bytes and directory entries (`dirent64_t`)

---

## 🔧 Build

Use a modern C compiler:

```bash
# Builder
gcc -O2 -std=c17 -Wall -Wextra mkfs_builder.c -o mkfs_builder

# Adder
gcc -O2 -std=c17 -Wall -Wextra mkfs_adder.c   -o mkfs_adder
```

> Some environments may require linking with `-static` or adding platform headers—follow your spec.

---

## 🚀 Usage

### 1) Create a fresh image

```bash
mkfs_builder \
  --image out.img \
  --size-kib <180..4096> \
  --inodes  <128..512>
```

**Parameters**

* `--image`   : output **.img** file to create
* `--size-kib`: total image size in **KiB** (multiple of 4, between 180 and 4096)
* `--inodes`  : number of inodes (between 128 and 512)

What it does:

* Lays out superblock, bitmaps, inode table, data region
* Initializes root inode (`/`) and writes `.` and `..` dirents
* Zeros everything else
* Computes and writes checksums where required

### 2) Add a file to the root directory

```bash
mkfs_adder \
  --input  out.img \
  --output out2.img \
  --file   <host-file-path>
```

**Parameters**

* `--input` : existing MiniVSFS image
* `--output`: new image to write after modification (won’t overwrite input)
* `--file`  : a **regular** host file to copy into `/` of the image

What it does:

* Loads the image into memory
* Finds a free inode + enough free data blocks (bitmaps)
* Copies the host file’s bytes into the data region
* Writes file inode (mode, size, times, direct pointers)
* Inserts a directory entry in `/` with the file name
* Updates root size and superblock timestamp
* Writes the new image to `--output`

> File name must fit in 58 bytes (no `/`, not `.` or `..`).
> Max file size ≈ `12 * 4096 = 49,152 bytes` (12 direct blocks, no indirects).

---

## 🧪 Quick Start Example

```bash
# 1) Build an empty FS image
./mkfs_builder --image out.img --size-kib 256 --inodes 128

# 2) Add a host file into the root of the image
./mkfs_adder --input out.img --output out2.img --file hello.txt
```

Now `out2.img` contains `/hello.txt`.

---

## 🛡️ Constraints & Checks

* **Geometry**: `--size-kib` must be in `[180, 4096]` and divisible by 4
* **Inodes**: `[128, 512]`
* **Filename**: ≤ 58 bytes, no slashes, not `.` or `..`
* **File size**: ≤ `DIRECT_MAX * BS` (12 × 4096)
* **Root-only**: no subdirectories
* **Checksums**: superblock and inode checks must be finalized after all fields are set

---

## 🧰 Troubleshooting

* **“Segmentation fault” in adder**
  Often caused by a malformed image: wrong write order, empty bitmaps, or off-by-one in the root inode index. Rebuild the image and re-run.

* **Image doesn’t look right?**
  Inspect blocks with `hexdump`:

  ```bash
  # superblock (block 0)
  dd if=out.img bs=4096 skip=0 count=1 | hexdump -C | head
  # inode bitmap (block 1), first byte should be 0x01
  dd if=out.img bs=4096 skip=1 count=1 | hexdump -C | head
  # data bitmap (block 2), first byte should be 0x01
  dd if=out.img bs=4096 skip=2 count=1 | hexdump -C | head
  ```

* **Adder says “Root dir full”**
  Directory block is full of entries (each 64 bytes). You may need to raise limits or implement expansion per your spec (MiniVSFS often keeps it simple).

---

## 🧭 Design Notes (MiniVSFS)

* **Why bitmaps?** Simple & fast free-space tracking (bit per inode/block).
* **Why only root (`/`)?** Keeps directory logic minimal for learning.
* **No indirects?** Max file size = `direct_count × block_size`, great for small tests.
* **Byte-exact**: deterministic, easy to grade, easy to diff.

---

## 📜 License & Credits

Use any license that matches your course or project requirements.
Concept inspired by “VSFS” (Very Simple File System) from teaching materials.
**Please refer to the codes and original project specification for any inquiry.**

---

## 🙌 Contributing

Issues and PRs are welcome—tests, sanity checks, or doc improvements all help. If you expand beyond root-only or add indirect blocks, call it out clearly in the README.

---

Happy forging! 🧱🔥
