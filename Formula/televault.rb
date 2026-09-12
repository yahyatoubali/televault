class Televault < Formula
  desc "Encrypted, high-performance cloud storage powered by Telegram (C++23)"
  homepage "https://github.com/yahyatoubali/televault"
  version "4.0.3"
  license "MIT"

  # Prebuilt release binaries (fast install). Per-asset sha256 values are
  # filled in right after the vX.Y.Z release workflow publishes them; until
  # then the matching `url`+`sha256` pair below must be updated together.
  on_macos do
    on_arm do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-darwin-arm64.tar.gz"
      sha256 "de3a67653f091f2cb4d9e94fe59783c13da7d9b63be8a5a0803966447e59912c"
    end
    on_intel do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-darwin-x86_64.tar.gz"
      sha256 "2e571d526041360b60b114d73937485d5b7c1402689204fe456cf312e014f2b3"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-linux-x86_64.tar.gz"
      sha256 "17b4af63383edbf824d7d0902e53a79dfef707c3f82b11e916a9e782eb6075b3"
    end
    on_arm do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-linux-aarch64.tar.gz"
      sha256 "c1f00414f3603a1377a7cf28482cac65fdf9b8e58a02f30253c1cf5da45d2a93"
    end
  end

  def install
    bin.install "televault"
    bin.install_symlink "televault" => "tvt"
  end

  test do
    assert_match "TeleVault", shell_output("#{bin}/televault --version")
    assert_match "TeleVault", shell_output("#{bin}/tvt --version")
  end
end
