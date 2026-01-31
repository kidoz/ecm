class Ecm < Formula
  desc "Error Code Modeler - encoder/decoder for CD image ECC/EDC data"
  homepage "https://github.com/kidoz/ecm"
  url "https://github.com/kidoz/ecm/archive/v1.3.0.tar.gz"
  sha256 "daa079c43d0895dba2cf6c4b0ee309bba909745e4bd194121da3c80410aab5ac"
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
