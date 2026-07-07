#!/bin/sh
# Copyright (c) 2014-2021 The Bitcoin Core developers
# Copyright (c) 2010-2024 The Freicoin Developers
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of version 3 of the GNU Affero General Public License as published
# by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
# details.
#
# You should have received a copy of the GNU Affero General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

export LC_ALL=C
if [ -z "$OSSLSIGNCODE" ]; then
  OSSLSIGNCODE=osslsigncode
fi

if [ "$#" -ne 1 ]; then
  echo "usage: $0 <path to key>"
  echo "example: $0 codesign.key"
  exit 1
fi

OUT=signature-win.tar.gz
SRCDIR=unsigned
WORKDIR=./.tmp
OUTDIR="${WORKDIR}/out"
OUTSUBDIR="${OUTDIR}/win"
TIMESERVER=http://timestamp.comodoca.com
CERTFILE="win-codesign.cert"

stty -echo
printf "Enter the passphrase for %s: " "$1"
read cs_key_pass
printf "\n"
stty echo


mkdir -p "${OUTSUBDIR}"
find ${SRCDIR} -wholename "*.exe" -type f -exec realpath --relative-to=. {} \; | while read -r bin
do
    echo Signing "${bin}"
    bin_base="$(realpath --relative-to=${SRCDIR} "${bin}")"
    mkdir -p "$(dirname ${WORKDIR}/"${bin_base}")"
    "${OSSLSIGNCODE}" sign -certs "${CERTFILE}" -t "${TIMESERVER}" -h sha256 -in "${bin}" -out "${WORKDIR}/${bin_base}" -key "$1" -pass "${cs_key_pass}"
    mkdir -p "$(dirname ${OUTSUBDIR}/"${bin_base}")"
    "${OSSLSIGNCODE}" extract-signature -pem -in "${WORKDIR}/${bin_base}" -out "${OUTSUBDIR}/${bin_base}.pem" && rm "${WORKDIR}/${bin_base}"
done

rm -f "${OUT}"
tar -C "${OUTDIR}" -czf "${OUT}" .
rm -rf "${WORKDIR}"
echo "Created ${OUT}"
