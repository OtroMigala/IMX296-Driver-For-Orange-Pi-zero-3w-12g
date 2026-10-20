# Entorno de compilación cruzada de módulos para la Orange Pi Zero 3W (kernel 6.6.98-sun60iw2).
# - Headers: el mismo paquete que trae la imagen oficial 1.0.2 (/opt/linux-headers-current-sun60iw2_1.0.0_arm64.deb).
#   Sus binarios de scripts/ (fixdep, modpost) son x86-64, por eso el contenedor es x86-64.
# - Compilador: Arm GNU Toolchain 11.2-2022.02, el mismo con que se compiló el kernel de la imagen.
# Ver README.md, sección "Compilación".
FROM ubuntu:22.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      make gcc libc6-dev bc flex bison libelf-dev libssl-dev \
      xz-utils wget ca-certificates kmod file binutils \
 && rm -rf /var/lib/apt/lists/*

ARG TC=gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu
RUN wget -q https://developer.arm.com/-/media/Files/downloads/gnu/11.2-2022.02/binrel/${TC}.tar.xz \
 && tar -xf ${TC}.tar.xz -C /opt \
 && rm ${TC}.tar.xz
ENV PATH=/opt/${TC}/bin:$PATH

COPY linux-headers-current-sun60iw2_1.0.0_arm64.deb /tmp/
# Se extrae aparte: extraerlo sobre / reemplaza los enlaces /bin -> /usr/bin de Ubuntu y rompe el contenedor.
RUN mkdir /tmp/h \
 && dpkg -x /tmp/linux-headers-current-sun60iw2_1.0.0_arm64.deb /tmp/h \
 && mv /tmp/h/usr/src/linux-headers-6.6.98-sun60iw2 /usr/src/ \
 && rm -rf /tmp/h /tmp/linux-headers-current-sun60iw2_1.0.0_arm64.deb

ENV KDIR=/usr/src/linux-headers-6.6.98-sun60iw2 \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-none-linux-gnu-

# Perl completo para $KDIR/scripts/checkpatch.pl (revisión de estilo del driver) y patch para
# aplicar driver/vin/*.patch (compilar_vin.sh).
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends perl patch \
 && rm -rf /var/lib/apt/lists/*

COPY compilar.sh /usr/local/bin/compilar.sh
RUN chmod +x /usr/local/bin/compilar.sh
WORKDIR /tmp
