# Easiest: double-click build.command in this same folder - it builds
# both architectures below and drops both .eap files directly into the
# project root automatically, then bumps the version in manifest.json
# ready for the next build.
#
# The AXIS C1710 (C17 Series Network Display Speaker) runs an NXP i.MX
# 8M Mini, i.e. aarch64 - that's the build you want for a C17. armv7hf
# is built too so the same package can be installed on older/smaller
# Axis speakers (e.g. a C1210) for testing the non-display parts.
#
# Manual build (aarch64, the C17):
#   docker build --platform=linux/amd64 --build-arg ARCH=aarch64 --tag c17timer-aarch64 .
#
# For an armv7hf device instead:
#   docker build --platform=linux/amd64 --tag c17timer-armv7hf .
#
# This only builds the image - it doesn't copy the .eap back out on its
# own. Extract it directly into the project root with docker create +
# docker cp (no separate build-output-*/ folder - see README.md's
# "Building" section for the full one-liners and current .eap filename):
#   docker create --name c17-extract --platform=linux/amd64 <tag>
#   docker cp c17-extract:"/opt/app/<name>.eap" "./<name>.eap"
#   docker rm c17-extract
ARG ARCH=armv7hf
ARG VERSION=12.10.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

WORKDIR /opt/app
COPY ./app .
RUN . /opt/axis/acapsdk/environment-setup* && \
    acap-build .
