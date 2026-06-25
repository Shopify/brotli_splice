require "mkmf"

# Find brotli headers and libraries (Homebrew or system)
dir_config("brotli",
  ["/opt/homebrew/include", "/usr/local/include"],
  ["/opt/homebrew/lib", "/usr/local/lib"])

abort "missing brotli/encode.h" unless have_header("brotli/encode.h")
abort "missing brotli/decode.h" unless have_header("brotli/decode.h")
abort "missing libbrotlienc"    unless have_library("brotlienc")
abort "missing libbrotlidec"    unless have_library("brotlidec")
abort "missing libbrotlicommon" unless have_library("brotlicommon")

create_makefile("brotli_splice/brotli_splice")
