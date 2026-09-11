# Maintainer: Yahya Toubali <yahyatoubali@example.com>
pkgname=televault-bin
pkgver=4.0.2
pkgrel=1
pkgdesc="End-to-end encrypted cloud filesystem backed by unlimited Telegram storage (pre-compiled binary)"
arch=('x86_64' 'aarch64')
url="https://github.com/yahyatoubali/televault"
license=('Apache-2.0')
provides=('televault')
conflicts=('televault')
depends=('glibc' 'fuse3')
source=("https://github.com/yahyatoubali/televault/releases/download/v${pkgver}/televault-v${pkgver}-linux-${CARCH}.tar.gz")
sha256sums=('SKIP')

package() {
    cd "${srcdir}/televault-v${pkgver}-linux-${CARCH}" || cd "${srcdir}"
    install -Dm755 televault "${pkgdir}/usr/bin/televault"
    ln -s televault "${pkgdir}/usr/bin/tvt"
    if [ -f LICENSE ]; then
        install -Dm644 LICENSE "${pkgdir}/usr/share/licenses/${pkgname}/LICENSE"
    fi
}
