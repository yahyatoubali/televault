class Televault < Formula
  desc "Encrypted, high-performance cloud storage powered by Telegram"
  homepage "https://github.com/yahyatoubali/televault"
  url "https://github.com/yahyatoubali/televault/archive/refs/tags/v4.0.0.tar.gz"
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
    ]

    system "cmake", "-S", ".", "-B", "build", *args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    assert_match "TeleVault", shell_output("#{bin}/tvt --version")
  end
end
