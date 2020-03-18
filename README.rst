# Kernel module for stress-testing imx scu

Module must be build out-of-tree, for example:

```bash
    . /opt/environment-setup-arm64 
    make -C ~/files/work/linux-kernel-build/source O=~/files/work/linux-kernel-build SUBDIRS=$(readlink -f .) M=$(readlink -f .) modules
```

Tries to be compatible with old imx kernels but some symbols need to be
explicitly exported.
