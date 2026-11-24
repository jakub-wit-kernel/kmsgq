# Building and testing on Windows + WSL2

The WSL2 kernel is Microsoft's own build, so there is no
`linux-headers-$(uname -r)` package for it. To build an out-of-tree module
you build a matching kernel tree from Microsoft's source and point `KDIR`
at it.

> These steps follow the standard WSL2 kernel-source workflow but have not
> yet been run end to end for this project. Note anything that differs on
> your machine and fix this file.

## 0. Prerequisites

Work inside the Linux filesystem (`~/...`), not under `/mnt/c`. Builds there
are much faster and Unix permissions behave normally.

```bash
sudo apt update
sudo apt install -y build-essential flex bison bc libssl-dev libelf-dev \
    dwarves python3 git valgrind clang clang-tidy
uname -r          # e.g. 6.6.87.2-microsoft-standard-WSL2
```

## 1. Get the kernel source that matches `uname -r`

Microsoft tags releases as `linux-msft-wsl-<version>`. For
`6.6.87.2-microsoft-standard-WSL2`, the tag is `linux-msft-wsl-6.6.87.2`.

```bash
VER=$(uname -r | sed 's/-microsoft.*//')
git clone --depth 1 --branch "linux-msft-wsl-$VER" \
    https://github.com/microsoft/WSL2-Linux-Kernel.git ~/wsl-kernel
```

If the clone fails, list the tags with
`git ls-remote --tags https://github.com/microsoft/WSL2-Linux-Kernel.git`
and pick the one that matches your version.

## 2. Configure with the running kernel's config and build it

```bash
cd ~/wsl-kernel
zcat /proc/config.gz > .config        # or: cp Microsoft/config-wsl .config
make olddefconfig
make -j"$(nproc)"                     # ~10-30 min; produces Module.symvers
```

A full build (not just `modules_prepare`) generates `Module.symvers`. Without
it, modpost cannot record symbol CRCs, and loading can fail with "disagrees
about version of symbol". The `CONFIG_LOCALVERSION` in the copied config
keeps the module's vermagic equal to `uname -r`.

## 3. Build and test kmsgq

```bash
git clone https://github.com/jakub-wit-kernel/kmsgq.git ~/kmsgq   # after publishing
cd ~/kmsgq
make check                                # user-space tests, no kernel needed
make KDIR=~/wsl-kernel                    # build kmsgq.ko
modinfo ./kmsgq.ko | grep vermagic        # must match `uname -r`
sudo KDIR="$HOME/wsl-kernel" scripts/run-integration.sh
```

The `$HOME` in the last command expands before `sudo` runs, so it points at
your home directory, not root's.

## 4. Manual exploration

```bash
sudo scripts/load.sh nr_devs=2 capacity=8192 max_msg=512
sudo sh -c 'printf hello > /dev/kmsgq0; printf world > /dev/kmsgq0'
sudo dd if=/dev/kmsgq0 bs=64 count=1 status=none; echo   # -> hello (one read = one message)
sudo dd if=/dev/kmsgq0 bs=64 count=1 status=none; echo   # -> world
sudo dmesg | grep kmsgq
sudo scripts/unload.sh
```

## VS Code

Install the **WSL** extension, then run `code .` from `~/kmsgq` inside WSL.
For accurate IntelliSense on the driver, point the C/C++ extension's include
path at `~/wsl-kernel/include`, `~/wsl-kernel/arch/x86/include` and
`~/wsl-kernel/arch/x86/include/generated`, and define `__KERNEL__`.

## If loading fails

| Message | Cause | Fix |
|---------|-------|-----|
| `Invalid module format` | vermagic does not match the running kernel | rebuild `~/wsl-kernel` from the tag and config of the *running* kernel |
| `disagrees about version of symbol` | `Module.symvers` missing or stale | full `make` in `~/wsl-kernel` (step 2) |
| `Operation not permitted` | not root | use `sudo` |
| `/dev/kmsgq0 did not appear` | no devtmpfs/udev node | check `ls /sys/class/kmsgq`; create the node by hand with `mknod` using the major number from `dmesg` |

An alternative that avoids building the WSL kernel: test inside a Linux VM
that runs a stock distribution kernel, such as a Hyper-V or VirtualBox
Ubuntu guest. There, `sudo apt install linux-headers-$(uname -r)` works
directly.
