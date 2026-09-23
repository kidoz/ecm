class Ecm < Formula
  desc "Error Code Modeler - encoder/decoder for CD image ECC/EDC data"
  homepage "https://github.com/kidoz/ecm"
  url "https://github.com/kidoz/ecm/archive/v1.3.4.tar.gz"
  sha256 "c31e6456b1c487b74d714c883026bcb27fd9fce026f5703b034de5c4092970f5"
  license "GPL-2.0-or-later"
  head "https://github.com/kidoz/ecm.git", branch: "master"

  depends_on "meson" => :build
  depends_on "ninja" => :build

  # Requires C23 support
  on_macos do
    depends_on xcode: ["15.0", :build]
  end

  def install
    system "meson", "setup", "build", *std_meson_args
    system "meson", "compile", "-C", "build"
    bin.install "build/ecm"
    bin.install "build/unecm"
  end

  test do
    assert_match "v#{version}", shell_output("#{bin}/ecm 2>&1", 1)
    assert_match "v#{version}", shell_output("#{bin}/unecm 2>&1", 1)
  end
end
