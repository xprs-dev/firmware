# Build image for repacking the D340W firmware: mtd-utils 2.0.2 (matches the
# camera's Linux 4.19 era), pakler, ubi_reader, python-lzo.
#   docker build -t reobell-mtd models/reolink-d340w/firmware/tool
# Run build steps with:  --cap-add MKNOD  (ubireader needs it)
# Run the nandsim mount-test with:  --privileged -v /lib/modules:/lib/modules:ro
FROM debian:12
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -qq && apt-get install -y -qq \
    build-essential autoconf automake libtool pkg-config \
    liblzo2-dev zlib1g-dev uuid-dev libssl-dev wget bzip2 ca-certificates \
    python3 python3-pip xxd kmod mtd-utils cpio >/dev/null
RUN pip install -q --break-system-packages pakler ubi_reader python-lzo
RUN cd /tmp && wget -q -O mtd.tar.bz2 https://infraroot.at/pub/mtd/mtd-utils-2.0.2.tar.bz2 && \
    mkdir mtd && tar xf mtd.tar.bz2 -C mtd --strip-components=1 && cd mtd && \
    (./autogen.sh >/dev/null 2>&1 || autoreconf -fi >/dev/null 2>&1) && \
    ./configure --without-tests --disable-tests >/dev/null 2>&1 && \
    make -j4 >/dev/null 2>&1 && cp mkfs.ubifs ubinize /usr/local/bin/ && \
    mkfs.ubifs --version && ubinize --version
