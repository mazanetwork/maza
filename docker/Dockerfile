FROM debian:stretch
LABEL maintainer="Maza Developers <dev@mazacoin.org>"
LABEL description="Dockerised MazaCore, built from Travis"

RUN apt-get update && apt-get -y upgrade && apt-get clean && rm -fr /var/cache/apt/*

COPY bin/* /usr/bin/
