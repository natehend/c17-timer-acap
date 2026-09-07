# Easiest: double-click build.command in this same folder - it builds the
# .eap and drops it directly into the project root automatically, then
# bumps the version in manifest.json ready for the next build.
#
# The whole C17 Series (C1710, C1720) runs an NXP i.MX 8M Mini, i.e.
# aarch64, so that is the only architecture this app is built for. See
# Axis's device list at https://help.axis.com/en-us/axis-audio-manager-pro
# under "Prepare your devices", item 4.
#
# Manual build:
#   docker build --platform=linux/amd64 --tag c17timer-aarch64 .
#
# This only builds the image - it doesn't copy the .eap back out on its
# own. Extract it directly into the project root with docker create +
# docker cp (no separate build-output-*/ folder - see README.md's
# "Building" section for the full one-liners and current .eap filename):
#   docker create --name c17-extract --platform=linux/amd64 <tag>
#   docker cp c17-extract:"/opt/app/<name>.eap" "./<name>.eap"
#   docker rm c17-extract
ARG ARCH=aarch64
ARG VERSION=12.10.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

WORKDIR /opt/app
COPY ./app .
RUN . /opt/axis/acapsdk/environment-setup* && \
    acap-build .
