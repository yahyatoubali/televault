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
      sha256 "d7697e7fb3aebba0401802114a8fa1c6248fe7cec356935d320a7592ff9ba130"
    end
    on_intel do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-darwin-x86_64.tar.gz"
      sha256 "4ae73ab42a09d99e9efc017c9dbef2d394774f3de84c396488e161745b8f66ed"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-linux-x86_64.tar.gz"
      sha256 "26d7d0f11593223e9d33de69caab1ffe82ef16b5e06d30c6c58fbbc823c8c3dc"
    end
    on_arm do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-linux-aarch64.tar.gz"
      sha256 "9b58546f8188a7a543d57d147a8acde9bbf3de9f2338cd63045dcdfa49265e20"
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
