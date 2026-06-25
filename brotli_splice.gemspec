# frozen_string_literal: true

require_relative "lib/brotli_splice/version"

Gem::Specification.new do |spec|
  spec.name = "brotli_splice"
  spec.version = BrotliSplice::VERSION
  spec.summary = "Create Brotli streams with fixed-length spliceable slots"
  spec.description = <<~DESC
    BrotliSplice is a Ruby C extension that creates Brotli-compressed streams
    containing a fixed-length uncompressed slot. The slot can be overwritten
    with a simple byte copy, making it useful for injecting fixed-size secrets
    or tokens into pre-compressed HTML responses.
  DESC

  spec.authors = ["Shopify"]
  spec.email = ["gems@shopify.com"]
  spec.homepage = "https://github.com/Shopify/brotli_splice"
  spec.license = "MIT"

  spec.required_ruby_version = ">= 3.1"
  spec.require_paths = ["lib"]
  spec.extensions = ["ext/brotli_splice/extconf.rb"]

  spec.metadata["allowed_push_host"] = "https://rubygems.org"
  spec.metadata["homepage_uri"] = spec.homepage
  spec.metadata["source_code_uri"] = "#{spec.homepage}/tree/main"
  spec.metadata["changelog_uri"] = "#{spec.homepage}/releases"
  spec.metadata["rubygems_mfa_required"] = "true"

  spec.files = Dir.chdir(__dir__) do
    Dir[
      "lib/**/*.rb",
      "ext/brotli_splice/**/*.{c,h,rb}",
      "README.md",
      "LICENSE.md",
      "brotli_splice.gemspec",
    ].reject { |file| File.directory?(file) }
  end
end
