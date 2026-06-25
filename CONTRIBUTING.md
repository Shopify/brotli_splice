# Contributing to BrotliSplice

Bug reports and pull requests are welcome on GitHub.

## Contributor License Agreement

External contributors must sign Shopify's Contributor License Agreement before
their pull requests can be merged. The CLA check will guide contributors through
the signing flow when needed.

## Development

Install the Brotli C library before building the native extension.

```sh
brew install brotli
gem build brotli_splice.gemspec
gem install ./brotli_splice-*.gem
ruby -e 'require "brotli_splice"; puts BrotliSplice::VERSION'
```

For local extension development:

```sh
cd ext/brotli_splice
ruby extconf.rb
make
cd ../..
ruby -Ilib test_ext.rb
```

## Pull Requests

Keep changes focused and include a smoke test or reproduction where practical.
For native extension changes, verify that the packaged gem builds, installs,
and loads from a clean gem environment.
