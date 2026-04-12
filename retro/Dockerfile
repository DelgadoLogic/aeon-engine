# AeonBrowser Retro — Open Watcom 2.0 Build Container
# DelgadoLogic
#
# Builds the 32-bit DLL (aeon_html4.dll) using Open Watcom 2.0.
#
# USAGE:
#   docker build -t aeon-retro-build .
#   docker run --rm -v ${PWD}:/src aeon-retro-build

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    wget xz-utils ca-certificates make \
    && rm -rf /var/lib/apt/lists/*

# Download Open Watcom 2.0 snapshot (Current-build) from GitHub
RUN mkdir -p /opt/watcom && \
    wget -q "https://github.com/open-watcom/open-watcom-v2/releases/download/Current-build/ow-snapshot.tar.xz" \
         -O /tmp/ow.tar.xz && \
    tar xf /tmp/ow.tar.xz -C /opt/watcom && \
    rm /tmp/ow.tar.xz

# Set Open Watcom environment variables
ENV WATCOM=/opt/watcom
ENV PATH="${WATCOM}/binl64:${WATCOM}/binl:${PATH}"
ENV INCLUDE="${WATCOM}/h:${WATCOM}/h/nt:${WATCOM}/h/nt/directx:${WATCOM}/h/win"
ENV LIB="${WATCOM}/lib386/nt:${WATCOM}/lib386:${WATCOM}/lib286/win:${WATCOM}/lib286"
ENV EDPATH="${WATCOM}/eddat"
ENV WIPFC="${WATCOM}/wipfc"

WORKDIR /src

# Build 32-bit DLL by default
CMD ["wmake", "-f", "makefile_32bit"]
