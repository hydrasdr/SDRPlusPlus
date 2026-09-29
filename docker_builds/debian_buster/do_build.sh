#!/bin/bash
set -e
cd /root

# Install dependencies and tools
apt update
apt install -y build-essential cmake git libfftw3-dev libglfw3-dev libvolk1-dev libzstd-dev \
            librtaudio-dev libhackrf-dev librtlsdr-dev libbladerf-dev liblimesuite-dev portaudio19-dev \
            libcodec2-dev

# Build and install libhydrasdr
git clone https://github.com/hydrasdr/hydrasdr-host hydrasdr-host
cd hydrasdr-host
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
make -j2
make install
cd ../../

cd SDRPlusPlus
mkdir build
cd build
cmake .. -DOPT_BUILD_BLADERF_SOURCE=OFF -DOPT_BUILD_LIMESDR_SOURCE=ON -DOPT_BUILD_NEW_PORTAUDIO_SINK=ON -DOPT_BUILD_M17_DECODER=ON -DOPT_BUILD_HYDRASDR_RFONE_SOURCE=ON
make VERBOSE=1 -j2

cd ..
sh make_debian_package.sh ./build 'libfftw3-dev, libglfw3-dev, libvolk1-dev, librtaudio-dev, libzstd-dev'