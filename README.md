CStar Compiler


Compiling From Source

You will need LLVM installed you can try to follow the instructions on the website.  This worked best for me:

```
git clone http://llvm.org/git/llvm.git
git clone http://llvm.org/git/clang.git llvm/tools/clang
git clone http://llvm.org/git/clang-tools-extra.git llvm/tools/clang/tools/extra
git clone http://llvm.org/git/compiler-rt.git llvm/projects/compiler-rt
git clone http://llvm.org/git/libcxx.git llvm/projects/libcxx
git clone http://llvm.org/git/libcxxabi.git llvm/projects/libcxxabi

mkdir build_llvm
cd build_llvm && cmake -G "Unix Makefiles" -DCMAKE_INSTALL_PREFIX=prefix=/usr/local/llvm ../llvm
make
```

This may take several hours to build.

You can then create a nice update script to keep your LLVM up to date:

```
#!/bin/bash

root=$(pwd)
cd $root/llvm && git pull --rebase origin master
cd $root/llvm/tools/clang && git pull --rebase origin master
cd $root/llvm/tools/clang/tools/extra && git pull --rebase origin master
cd $root/llvm/projects/compiler-rt && git pull --rebase origin master
cd $root/llvm/projects/libcxx && git pull --rebase origin master
cd $root/llvm/projects/libcxxabi && git pull --rebase origin master
```

The current MakeFile assumes you have installed LLVM under: /usr/local/llvm/include