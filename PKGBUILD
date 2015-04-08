pkgname=brightnessd-git
pkgver=0.1
pkgrel=1
pkgdesc="regulate screen brightness depending on user input (in)activity"
arch=('i686' 'x86_64')
url="https://github.com/stormc/brightnessd"
license=('MIT')
depends=('libxcb')
makedepends=('git')
source=("$pkgname"::'git+https://github.com/stormc/brightnessd.git')
md5sums=('SKIP')

build() {
  cd "$srcdir/$pkgname"
  make
}

package() {
  cd "$srcdir/$pkgname"
  make PREFIX=/usr DESTDIR="$pkgdir" install
}
