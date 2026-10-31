#!/bin/bash


export PATH="$PATH:/home/juezhi/proton-clang/bin"
export ARCH=arm64
export SUBARCH=arm64
export CC=clang
export CLANG_TRIPLE=aarch64-linux-gnu-
export CROSS_COMPILE=aarch64-linux-gnu-
export CROSS_COMPILE_ARM32=arm-linux-gnueabi-
export LLVM=1
export LLVM_IAS=1

make O=out ARCH=arm64   vendor/xiaomi/mi8998_defconfig

make -j24 O=out ARCH=arm64 \
CC=clang \
AR=llvm-ar \
NM=llvm-nm \
OBJCOPY=llvm-objcopy \
OBJDUMP=llvm-objdump \
STRIP=llvm-strip \
KCFLAGS="-Wno-error"
