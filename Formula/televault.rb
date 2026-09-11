class Televault < Formula
  desc "Encrypted, high-performance cloud storage powered by Telegram (C++23)"
  homepage "https://github.com/yahyatoubali/televault"
  version "4.0.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-darwin-arm64.tar.gz"
    end
    on_intel do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-darwin-x86_64.tar.gz"
    end
  end

  on_linux do
    on_intel do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-linux-x86_64.tar.gz"
    end
    on_arm do
      url "https://github.com/yahyatoubali/televault/releases/download/v#{version}/televault-v#{version}-linux-aarch64.tar.gz"
    end
  end

  def install
    bin.install "televault"
    bin.install_symlink "televault" => "tvt"
  end

  test do
    assert_match "TeleVault", shell_output("#{bin}/tvt --version")
  end
end
