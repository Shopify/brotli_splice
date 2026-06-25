# BrotliSplice

A Ruby C extension that produces Brotli-compressed streams with a fixed-length
**spliceable slot** — a region whose raw bytes can be overwritten without
decompressing or re-encoding the rest of the stream.

## Use case

You have a large HTML document with a small secret (token, nonce, personalized
data) embedded at a known position. You want to:

1. Brotli-compress the document **once** at build/deploy time
2. Swap in a different secret **per request** with a simple `memcpy`

## How it works

The stream is split into three chunks:

```
[chunk1: compressed part1]  [chunk2: uncompressed slot]  [chunk3: compressed part3]
         (FLUSH)               (raw bytes)              (STREAM_OFFSET + FINISH)
```

- **chunk1** — standard Brotli compression of everything before the secret
- **chunk2** — an uncompressed meta-block (3-byte header + raw data), whose
  bytes sit at a known offset in the stream
- **chunk3** — standard Brotli compression of everything after the secret,
  produced by a second encoder with `BROTLI_PARAM_STREAM_OFFSET` so that
  distance calculations, the ring buffer, and the WBITS header are all correct

The last 2 bytes of the secret slot are reserved as a fixed **context suffix**
(`\r\n`). These are baked into chunk3 and cannot be changed — the Brotli encoder
uses the preceding 2 bytes as literal context when compressing part3. The
replaceable portion is therefore `secret_length - 2` bytes.

Compression overhead vs standard Brotli is small — typically 100–200 bytes
for realistic HTML documents. Two sources of overhead:

1. **FLUSH boundary** — the encoder can't carry pending matches from part1
   across the slot. A very small part1 may show proportionally higher
   overhead since this cost isn't amortized.
2. **Part3 can't back-reference part1** — `STREAM_OFFSET` starts a fresh
   encoder with an empty hash table, so part3 compresses in isolation.
   If part1 and part3 share significant repeated content (e.g. identical
   nav markup in header and footer), this gap widens. For typical HTML
   where each section has plenty of self-repetition, the loss is small.

**Recommendation:** place the secret early in the document (e.g. in a
`<script>` tag right after `<head>`) so that part1 is small. This keeps
the FLUSH cost negligible and gives part3 nearly the entire document to
compress against itself.

## Installation

Requires the Brotli C library (`libbrotlienc`, `libbrotlidec`, `libbrotlicommon`).

Add the gem to your Gemfile:

```ruby
gem "brotli_splice"
```

```sh
# macOS (Homebrew)
brew install brotli

# Build and install the gem from this checkout
gem build brotli_splice.gemspec
gem install ./brotli_splice-*.gem
```

## Releasing

This gem is configured for public release on RubyGems.org.

After RubyGems Trusted Publishing is configured for this repository, publishing
a GitHub release runs the `Release` workflow and publishes the gem.

```sh
gem build brotli_splice.gemspec
gem push brotli_splice-*.gem
```

Alternatively, use Bundler's release task after tagging the version:

```sh
bundle exec rake release
```

For local extension development without installing the gem:

```sh
cd ext/brotli_splice
ruby extconf.rb
make
cd ../..
ruby -Ilib test_ext.rb
```

## API

### `BrotliSplice.encode(html, secret_offset, secret_length, quality: 11) → Hash`

Compress `html` with a spliceable slot at the given byte position.

**Parameters:**
- `html` — the full HTML string (binary encoding)
- `secret_offset` — byte offset where the replaceable section starts
- `secret_length` — total byte length of the replaceable section (must be > 2)
- `quality:` — Brotli quality level, 0–11 (default: 11)

**Returns** a Hash:
```ruby
{
  data:           String,   # the Brotli-compressed stream
  secret_offset:  Integer,  # byte offset of the replaceable data within the compressed stream
  secret_length:  Integer,  # replaceable byte count (= secret_length - 2)
  context_suffix: String,   # the 2 fixed context bytes ("\r\n")
}
```

### `BrotliSplice.replace(compressed_data, new_secret, secret_offset, secret_length) → String`

Replace the secret in a compressed stream. Returns a new string with the
swapped bytes. This is a pure `memcpy` — no compression or decompression.

**Parameters:**
- `compressed_data` — the Brotli stream from `encode`
- `new_secret` — replacement content, must be exactly `secret_length` bytes
- `secret_offset` — from the `encode` result
- `secret_length` — from the `encode` result

## Example

```ruby
require "brotli_splice"
require "brotli" # for verification

html = "<html>...TOKEN_PLACEHOLDER_HERE...</html>"
token_offset = html.index("TOKEN_PLACEHOLDER_HERE")
token_length = "TOKEN_PLACEHOLDER_HERE".bytesize + 2  # +2 for context suffix

# Compress once
result = BrotliSplice.encode(html, token_offset, token_length, quality: 11)

# Per-request: swap in the real token (must be result[:secret_length] bytes)
real_token = "user-specific-secret".ljust(result[:secret_length], "\0")
response = BrotliSplice.replace(result[:data], real_token,
                                 result[:secret_offset], result[:secret_length])

# The response is a valid Brotli stream ready to send
decoded = Brotli.inflate(response)
# => "<html>...user-specific-secret\0\0\r\n...</html>"
```

## Decoded output

The decoded stream is the original HTML with the secret region replaced by:

```
[replaceable bytes (secret_length - 2)] [context suffix "\r\n"]
```

The 2-byte context suffix replaces the last 2 bytes of the original secret
region. Design your placeholder to account for this (e.g. make it 2 bytes
longer, or place the suffix where a line break is natural).

## Limitations

- The replaceable slot must be ≤ 65534 bytes (64KB minus the 2-byte context)
- The replacement must be **exactly** `secret_length` bytes (no length changes)
- The last 2 bytes of the original secret region become `\r\n` in the output
- Only one spliceable slot per stream (encode with multiple slots would require
  chaining encoders)
