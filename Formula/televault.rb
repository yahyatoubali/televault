class Televault < Formula
  desc "Encrypted, high-performance cloud storage powered by Telegram"
  homepage "https://github.com/yahyatoubali/televault"
  url "https://github.com/yahyatoubali/televault/archive/refs/tags/v4.0.0.tar.gz"
  sha256 "795e5b3d03e1e8c8ed9cf7a4db6e0f382a77730371617242aaf5cf766867e9c5"
  license "MIT"
  head "https://github.com/yahyatoubali/televault.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "pkg-config" => :build
  depends_on "boost"
  depends_on "openssl@3"
  depends_on "zstd"

  def install
    args = std_cmake_args + %w[
      -DCMAKE_BUILD_TYPE=Release
      -DTV_BUILD_TESTS=OFF
      -DTV_BUILD_TUI=ON
      -DTV_BUILD_TDLIB=OFF
      -DTV_BUILD_FUSE=OFF
      -DTV_BUILD_WEBDAV=ON
    ]

    system "cmake", "-S", ".", "-B", "build", *args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
    # `cmake --install` now creates bin/tvt via install(CODE ...) in
    # src/CMakeLists.txt, but keep a belt-and-braces symlink for older
    # revisions / Cellar relocations.
    bin.install_symlink bin/"televault" => "tvt" unless (bin/"tvt").exist?
  end

  test do
    assert_match "TeleVault", shell_output("#{bin}/televault --version")
    assert_match "TeleVault", shell_output("#{bin}/tvt --version")
  end
end
