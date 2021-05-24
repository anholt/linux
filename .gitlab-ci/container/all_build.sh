#!/bin/bash

set -e
set -o xtrace

apt-get -y install ca-certificates
sed -i -e 's/http:\/\/deb/https:\/\/deb/g' /etc/apt/sources.list
echo 'deb https://deb.debian.org/debian buster main' >/etc/apt/sources.list.d/buster.list
apt-get update

apt-get -y install \
	flex \
	g++ \
	git \
    libncurses-dev \
    lld \
    make \
	python-is-python3
