#!/bin/sh
set -eu

APP=xiaomi-pen-status
VERSION=0.1.1
ARCH="$(dpkg --print-architecture)"
ROOT="$(pwd)"
PKGROOT="$(mktemp -d)"
OUT="${ROOT}/${APP}_${VERSION}_${ARCH}.deb"
chmod 755 "${PKGROOT}"

cleanup() {
	rm -rf "${PKGROOT}"
}
trap cleanup EXIT

qmake6
make

install -Dm755 "${ROOT}/${APP}" "${PKGROOT}/usr/bin/${APP}"
install -Dm644 "${ROOT}/${APP}.desktop" \
	"${PKGROOT}/usr/share/applications/${APP}.desktop"
mkdir -p "${PKGROOT}/etc/xdg/autostart"
sed 's/^Exec=.*/Exec=xiaomi-pen-status/' "${ROOT}/${APP}.desktop" \
	> "${PKGROOT}/etc/xdg/autostart/${APP}.desktop"
chmod 755 "${PKGROOT}/etc/xdg/autostart"
chmod 644 "${PKGROOT}/etc/xdg/autostart/${APP}.desktop"
install -Dm644 "${ROOT}/${APP}.svg" \
	"${PKGROOT}/usr/share/icons/hicolor/scalable/apps/${APP}.svg"

mkdir -p "${PKGROOT}/DEBIAN"
cat > "${PKGROOT}/DEBIAN/control" <<EOF
Package: ${APP}
Version: ${VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libqt6widgets6, libqt6svg6, libqt6network6
Maintainer: siergtc <i@4t.pw>
Description: Stylus status tray utility
 A small Qt tray utility that reports stylus placement, battery level,
 seating warnings, and wireless TX debug values from qcom_battmgr sysfs.
EOF

dpkg-deb --build --root-owner-group "${PKGROOT}" "${OUT}"
printf '%s\n' "${OUT}"
