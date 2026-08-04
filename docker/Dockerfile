# Single self-contained build/repack environment for the Benro Polaris
# libgphoto2 patcher.
#
# Why debian:9 (stretch)?  Its cross toolchain targets glibc 2.24 — the EXACT
# libc version on the Polaris camera board (HiSilicon Hi3559V200, Linux 4.9.37).
# A modern toolchain would emit references to glibc symbol versions the device
# does not have (e.g. the 2.34 libpthread merge) and the rebuilt driver would
# fail to load.  debian:9 also happens to ship GCC 6.3.0 — the same compiler
# family Benro/Snoppa used for the stock build.
FROM debian:9

ENV DEBIAN_FRONTEND=noninteractive

# stretch is archived — point apt at the archive and disable the expiry check.
RUN set -eux; \
    printf 'deb http://archive.debian.org/debian stretch main\n\
deb http://archive.debian.org/debian-security stretch/updates main\n' > /etc/apt/sources.list; \
    echo 'Acquire::Check-Valid-Until "false";' > /etc/apt/apt.conf.d/10no-check-valid; \
    echo 'APT::Get::Assume-Yes "true";' > /etc/apt/apt.conf.d/90assumeyes; \
    apt-get update; \
    apt-get install --no-install-recommends -y \
        gcc-arm-linux-gnueabi g++-arm-linux-gnueabi binutils-arm-linux-gnueabi \
        autoconf automake libtool libtool-bin pkg-config make gettext autopoint \
        gawk bison patch file xz-utils wget ca-certificates \
        mtd-utils patchelf \
        libexif-dev libltdl-dev zlib1g-dev libpopt-dev libusb-1.0-0-dev \
        qemu-user-static \
        python3 python3-dev python3-pip liblzo2-dev; \
    rm -rf /var/lib/apt/lists/*

# ubi_reader (faithful, permission-preserving UBIFS extraction) + helpers.
# debian:9 ships pip/setuptools too old to build these; pin to the last
# Python-3.5-compatible releases first.
RUN set -eux; \
    pip3 install --upgrade "pip==20.3.4" "setuptools==44.1.1" wheel; \
    pip3 install "ubi_reader==0.8.0" python-lzo pyelftools

COPY container/ /opt/patcher/
RUN chmod +x /opt/patcher/*.sh

# /in  : read-only mount of the user's stock FwPkt (folder or unzipped)
# /out : writable mount for the produced custom FwPkt + FwPkt.zip
WORKDIR /work
ENTRYPOINT ["/opt/patcher/patch.sh"]
