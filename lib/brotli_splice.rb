# frozen_string_literal: true

require_relative "brotli_splice/version"

begin
  require "brotli_splice/brotli_splice"
rescue LoadError => error
  begin
    require_relative "../ext/brotli_splice/brotli_splice"
  rescue LoadError
    raise error
  end
end
