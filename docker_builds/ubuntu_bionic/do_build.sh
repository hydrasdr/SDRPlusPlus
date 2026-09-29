#!/bin/bash
set -e
cd /root

# Update repos to get a more recent cmake version
apt update
apt install -y gpg wget
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ bionic main' | tee /etc/apt/sources.list.d/kitware.list >/dev/null
apt update

# Install dependencies and tools
apt install -y build-essential cmake git libfftw3-dev libglfw3-dev libvolk1-dev libzstd-dev \
            librtaudio-dev libhackrf-dev librtlsdr-dev libbladerf-dev liblimesuite-dev portaudio19-dev \
            libcodec2-dev libudev-dev

# Install a more recent libusb version
wget https://github.com/libusb/libusb/releases/download/v1.0.25/libusb-1.0.25.tar.bz2
tar -xvf libusb-1.0.25.tar.bz2
cd libusb-1.0.25
./configure
make -j2
make install
cd ..

# Build and install libhydrasdr
git clone https://github.com/hydrasdr/hydrasdr-host hydrasdr-host
cd hydrasdr-host
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
make -j2
make install
cd ../../

# Fix missing .pc file for codec2
echo 'prefix=/usr/' >> /usr/share/pkgconfig/codec2.pc
echo 'libdir=/usr/include/x86_64-linux-gnu/' >> /usr/share/pkgconfig/codec2.pc
echo 'includedir=/usr/include/codec2' >> /usr/share/pkgconfig/codec2.pc
echo 'Name: codec2' >> /usr/share/pkgconfig/codec2.pc
echo 'Description: A speech codec for 2400 bit/s and below' >> /usr/share/pkgconfig/codec2.pc
echo 'Requires:' >> /usr/share/pkgconfig/codec2.pc
echo 'Version: 0.7' >> /usr/share/pkgconfig/codec2.pc
echo 'Libs: -L/usr/include/x86_64-linux-gnu/ -lcodec2' >> /usr/share/pkgconfig/codec2.pc
echo 'Cflags: -I/usr/include/codec2' >> /usr/share/pkgconfig/codec2.pc

# Build SDR++ Itself
cd SDRPlusPlus
mkdir build
cd build
cmake .. -DOPT_BUILD_BLADERF_SOURCE=OFF -DOPT_BUILD_LIMESDR_SOURCE=ON -DOPT_BUILD_NEW_PORTAUDIO_SINK=ON -DOPT_OVERRIDE_STD_FILESYSTEM=ON -DOPT_BUILD_M17_DECODER=ON -DOPT_BUILD_HYDRASDR_RFONE_SOURCE=ON
make VERBOSE=1 -j2

# Generate package
cd ..
sh make_debian_package.sh ./build 'libfftw3-dev, libglfw3-dev, libvolk1-dev, librtaudio-dev, libzstd-dev'