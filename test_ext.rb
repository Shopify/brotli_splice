#!/usr/bin/env ruby
# frozen_string_literal: true

$LOAD_PATH.unshift File.join(__dir__, "lib")
require "brotli_splice"
require "brotli"

# Use the RFC from the experiment dir, or download it
rfc_path = File.join(__dir__, "rfc7932.txt")
unless File.exist?(rfc_path)
  rfc_path = File.join(__dir__, "..", "brotli_experiment", "rfc7932.txt")
end
rfc = File.read(rfc_path, mode: "rb")
secret_offset = 1000
secret_length = 64

puts "=== BrotliSplice C Extension ==="
puts "HTML: #{rfc.bytesize}B, secret at offset #{secret_offset}, length #{secret_length}"

# 1) Encode: full HTML + where the secret is
result = BrotliSplice.encode(rfc, secret_offset, secret_length, quality: 11)
compressed = result[:data]

puts "Compressed: #{compressed.bytesize}B"
puts "Secret slot: offset=#{result[:secret_offset]}, length=#{result[:secret_length]}"
puts "Context suffix: #{result[:context_suffix].inspect}"
puts

# Verify the encoded stream decodes correctly
decoded = Brotli.inflate(compressed)
# The decoded output = part1 + secret_body + context_suffix + part3
expected = rfc.byteslice(0, secret_offset) +
           rfc.byteslice(secret_offset, secret_length - 2) +
           result[:context_suffix] +
           rfc.byteslice(secret_offset + secret_length, rfc.bytesize - secret_offset - secret_length)
puts "Initial decode: #{decoded == expected ? '✓' : '✗'}"

# 2) Replace: just the compressed bytestream + new secret
puts
puts "=== Replacements ==="
5.times do |i|
  new_secret = "TOKEN_#{i}_".ljust(result[:secret_length], ".")
  modified = BrotliSplice.replace(compressed, new_secret,
                                   result[:secret_offset], result[:secret_length])
  decoded = Brotli.inflate(modified)
  expected = rfc.byteslice(0, secret_offset) +
             new_secret +
             result[:context_suffix] +
             rfc.byteslice(secret_offset + secret_length, rfc.bytesize - secret_offset - secret_length)
  puts "  Replace #{i}: #{decoded == expected ? '✓' : '✗'} (secret=#{new_secret[0,20].inspect}...)"
end

# Size comparison
std = Brotli.deflate(rfc, quality: 11)
puts
puts "Sizes:"
puts "  Original:        #{rfc.bytesize}B"
puts "  Standard Brotli: #{std.bytesize}B (#{(std.bytesize * 100.0 / rfc.bytesize).round(1)}%)"
puts "  Spliceable:      #{compressed.bytesize}B (#{(compressed.bytesize * 100.0 / rfc.bytesize).round(1)}%)"
puts "  Overhead:        #{compressed.bytesize - std.bytesize}B"
