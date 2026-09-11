class Televault < Formula
  desc "Encrypted, high-performance cloud storage powered by Telegram (C++23)"
  homepage "https://github.com/yahyatoubali/televault"
  version "4.0.2"
  license "MIT"

  # Prebuilt release binaries (fast install). Per-asset sha256 values are
  # filled in right after the vX.Y.Z release workflow publishes them; until
  # then the matching `url`+`sha256` pair below must be updated together.
  on_macos do
    on_arm do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-darwin-arm64.tar.gz"
      sha256 "REPLACE_WITH_DARWIN_ARM64_SHA256_AFTER_RELEASE"
    end
    on_intel do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-darwin-x86_64.tar.gz"
      sha256 "REPLACE_WITH_DARWIN_X86_64_SHA256_AFTER_RELEASE"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-linux-x86_64.tar.gz"
      sha256 "REPLACE_WITH_LINUX_X86_64_SHA256_AFTER_RELEASE"
    end
    on_arm do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-linux-aarch64.tar.gz"
      sha256 "REPLACE_WITH_LINUX_AARCH64_SHA256_AFTER_RELEASE"
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
