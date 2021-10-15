#!/bin/bash

export WORKDIR="$(pwd)"

export ARCH="arm64"
export CROSS_COMPILE="${WORKDIR}/../gcc-arm-10.2-2020.11-x86_64-aarch64-none-elf/bin/aarch64-none-elf-"

if [ x"${CORES}" = x"" ]; then
    CORES=4
fi

KNAME="g2l"

if [ x"$1" = x"mrprober" ]; then
   rm -rf "${WORKDIR}/build/${KNAME}" 2>/dev/null || true
fi

if [ ! -d "${WORKDIR}/build/${KNAME}" ]; then
    mkdir -p "${WORKDIR}/build/${KNAME}"
fi

if [ ! -f "build/${KNAME}/.config" ]; then
    cp arch/arm64/configs/novotech_defconfig "build/${KNAME}/.config"
fi

make O="build/${KNAME}" -j${CORES} Image
make O="build/${KNAME}" -j${CORES} modules firmware
make O="build/${KNAME}" renesas/r9a07g044l2-smarc.dtb
make O="build/${KNAME}" renesas/r9a07g044l2-rzg2l-novotech.dtb

rm -rf "deploy/${KNAME}" 2>/dev/null || true
mkdir -p "deploy/${KNAME}"
mkdir -p "deploy/${KNAME}/modules"
mkdir -p "deploy/${KNAME}/headers/usr/src/linux"

cp -v "build/${KNAME}/arch/arm64/boot/Image" "deploy/${KNAME}/"
cp -v "build/${KNAME}/arch/arm64/boot/dts/renesas/"*.dtb "deploy/${KNAME}/"

make O="build/${KNAME}" modules_install INSTALL_MOD_PATH="${WORKDIR}/deploy/${KNAME}/modules" INSTALL_MOD_STRIP=1
make O="build/${KNAME}" headers_install INSTALL_HDR_PATH="${WORKDIR}/deploy/${KNAME}/headers/usr/src/linux"

tar -czf deploy/${KNAME}/modules.tar.gz -C deploy/${KNAME}/modules . --owner=0 --group=0
tar -czf deploy/${KNAME}/headers.tar.gz -C deploy/${KNAME}/headers . --owner=0 --group=0
