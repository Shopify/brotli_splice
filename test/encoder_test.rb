# frozen_string_literal: true

require "minitest/autorun"
require "objspace"
require "brotli"
require_relative "../lib/brotli_splice"

class BrotliSpliceEncoderTest < Minitest::Test
  def test_streaming_round_trip
    encoder = BrotliSplice::Encoder.new(quality: 5, lgwin: 22)
    compressed = encoder.write(binary("<html><head>"))
    compressed << encoder.write(binary("<title>Store</title>"))
    compressed << encoder.slot(binary("live-token\r\n"))
    compressed << encoder.finish(binary("</head><body>Hello</body></html>"))

    assert_equal(
      "<html><head><title>Store</title>live-token\r\n</head><body>Hello</body></html>",
      Brotli.inflate(compressed),
    )
  end

  def test_replaces_only_the_slot_body
    encoder = BrotliSplice::Encoder.new(quality: 5)
    compressed = encoder.write(binary("prefix"))
    compressed << encoder.slot(binary("live-token\r\n"))
    compressed << encoder.finish(binary("suffix"))

    replacement = binary("cache-slot")
    replaced = BrotliSplice.replace(compressed, replacement, encoder.slot_offset, encoder.slot_length)

    assert_equal("prefixcache-slot\r\nsuffix", Brotli.inflate(replaced))
    assert_equal("prefixlive-token\r\nsuffix", Brotli.inflate(compressed))
  end

  def test_one_shot_offsets_are_bytes_when_prefix_contains_utf8
    prefix = "<title>Café 中文</title>"
    slot = "placeholder\r\n"
    suffix = "</head>"
    html = prefix + slot + suffix

    result = BrotliSplice.encode(html, prefix.bytesize, slot.bytesize, quality: 5)

    assert_equal(html.b, Brotli.inflate(result[:data]))
    replacement = binary("replacement".ljust(result[:secret_length], "."))
    replaced = BrotliSplice.replace(result[:data], replacement, result[:secret_offset], result[:secret_length])
    assert_equal((prefix + replacement + "\r\n" + suffix).b, Brotli.inflate(replaced))
  end

  def test_one_shot_api_matches_streaming_layout
    html = binary("prefixplaceholder\r\nsuffix")
    offset = html.index("placeholder")
    length = binary("placeholder\r\n").bytesize

    one_shot = BrotliSplice.encode(html, offset, length, quality: 5)
    encoder = BrotliSplice::Encoder.new(quality: 5)
    streaming = encoder.write(html.byteslice(0, offset))
    streaming << encoder.slot(html.byteslice(offset, length))
    streaming << encoder.finish(html.byteslice(offset + length..))

    assert_equal(one_shot[:secret_offset], encoder.slot_offset)
    assert_equal(one_shot[:secret_length], encoder.slot_length)
    assert_equal(Brotli.inflate(one_shot[:data]), Brotli.inflate(streaming))
  end

  def test_write_emits_decodable_stream_bytes_before_finish
    encoder = BrotliSplice::Encoder.new(quality: 5)

    first = encoder.write(binary("early bytes"))

    refute_empty(first)
    assert_equal("early bytestail", Brotli.inflate(first + encoder.finish(binary("tail"))))
  end

  def test_write_after_slot
    encoder = BrotliSplice::Encoder.new(quality: 5)
    compressed = encoder.write(binary("prefix"))
    compressed << encoder.slot(binary("live-token\r\n"))
    compressed << encoder.write(binary("middle"))
    compressed << encoder.finish(binary("suffix"))

    assert_equal("prefixlive-token\r\nmiddlesuffix", Brotli.inflate(compressed))
  end

  def test_many_small_writes_decode
    encoder = BrotliSplice::Encoder.new(quality: 5)
    compressed = binary("")
    binary("many tiny writes").each_byte do |byte|
      compressed << encoder.write(binary(byte.chr))
    end
    compressed << encoder.finish

    assert_equal("many tiny writes", Brotli.inflate(compressed))
  end

  def test_slot_can_be_the_first_operation
    encoder = BrotliSplice::Encoder.new(quality: 5)
    compressed = encoder.slot(binary("live-token\r\n"))
    compressed << encoder.finish(binary("suffix"))

    assert_equal("live-token\r\nsuffix", Brotli.inflate(compressed))
  end

  def test_empty_stream
    encoder = BrotliSplice::Encoder.new(quality: 5)
    compressed = encoder.write(binary(""))
    compressed << encoder.finish

    assert_equal("", Brotli.inflate(compressed))
  end

  def test_maximum_slot_size
    encoder = BrotliSplice::Encoder.new(quality: 5)
    slot = binary("x" * 65_536 + "\r\n")
    compressed = encoder.slot(slot)
    compressed << encoder.finish

    assert_equal(slot, Brotli.inflate(compressed))
    assert_equal(65_536, encoder.slot_length)
  end

  def test_output_larger_than_internal_buffer_decodes
    body = Random.new(1234).bytes(100_000)
    encoder = BrotliSplice::Encoder.new(quality: 5)
    compressed = encoder.write(body)
    compressed << encoder.finish

    assert_operator(compressed.bytesize, :>, 16_384)
    assert_equal(body, Brotli.inflate(compressed))
  end

  def test_finish_without_a_slot
    encoder = BrotliSplice::Encoder.new(quality: 5)
    compressed = encoder.write(binary("first"))
    compressed << encoder.finish(binary("second"))

    assert_equal("firstsecond", Brotli.inflate(compressed))
    assert_nil(encoder.slot_offset)
    assert_nil(encoder.slot_length)
  end

  def test_invalid_state_and_slot_inputs
    encoder = BrotliSplice::Encoder.new
    assert_raises(BrotliSplice::Error) { encoder.slot(binary("\r\n")) }
    assert_raises(BrotliSplice::Error) { encoder.slot(binary("bodyXY")) }
    assert_raises(BrotliSplice::Error) { encoder.slot(binary("x" * 65_537 + "\r\n")) }
    utf8_encoder = BrotliSplice::Encoder.new
    utf8_compressed = utf8_encoder.write("café") + utf8_encoder.finish
    assert_equal("café".b, Brotli.inflate(utf8_compressed))

    encoder.slot(binary("slot\r\n"))
    assert_raises(BrotliSplice::Error) { encoder.slot(binary("slot\r\n")) }
    encoder.finish
    assert_raises(BrotliSplice::Error) { encoder.write(binary("late")) }
  end

  def test_close_is_idempotent_and_terminal
    encoder = BrotliSplice::Encoder.new

    assert_nil(encoder.close)
    assert_nil(encoder.close)
    assert_raises(BrotliSplice::Error) { encoder.write(binary("late")) }
    assert_raises(BrotliSplice::Error) { encoder.slot(binary("late\r\n")) }
    assert_raises(BrotliSplice::Error) { encoder.finish }
  end

  def test_close_after_finish_is_a_no_op
    encoder = BrotliSplice::Encoder.new
    compressed = encoder.finish(binary("complete"))

    assert_nil(encoder.close)
    assert_nil(encoder.close)
    assert_equal("complete", Brotli.inflate(compressed))
  end

  def test_close_releases_native_memory
    encoder = BrotliSplice::Encoder.new(quality: 11, lgwin: 24)
    encoder.write(Random.new(5678).bytes(100_000))
    active_size = ObjectSpace.memsize_of(encoder)

    encoder.close

    assert_operator(active_size, :>, ObjectSpace.memsize_of(encoder))
  end

  def test_replaces_a_stream_generated_by_version_0_1_1
    # Generated with v0.1.1 from "legacy-prefixplaceholder\r\nlegacy-suffix" at quality 5.
    compressed = [
      "0b060000245a8a10464ae3884f18500008706c616365686f6c6465720800080d0a" \
      "6000086c65676163792d73756666697803",
    ].pack("H*")
    replaced = BrotliSplice.replace(compressed, binary("cached-slot"), 17, 11)

    assert_equal("legacy-prefixcached-slot\r\nlegacy-suffix", Brotli.inflate(replaced))
  end

  def test_reentrant_parameter_coercion_does_not_replace_native_state
    encoder = BrotliSplice::Encoder.allocate
    quality = Object.new
    quality.define_singleton_method(:to_int) do
      encoder.send(:initialize, quality: 5)
      5
    end

    assert_raises(BrotliSplice::Error) { encoder.send(:initialize, quality: quality) }
    assert_equal("safe", Brotli.inflate(encoder.finish(binary("safe"))))
  end

  def test_one_shot_rechecks_input_length_after_numeric_coercion
    html = binary("slot\r\n")
    length = Object.new
    length.define_singleton_method(:to_int) do
      html.replace("x")
      6
    end

    assert_raises(BrotliSplice::Error) { BrotliSplice.encode(html, 0, length) }
  end

  def test_invalid_encoder_parameters_are_rejected
    assert_raises(BrotliSplice::Error) { BrotliSplice::Encoder.new(quality: -1) }
    assert_raises(BrotliSplice::Error) { BrotliSplice::Encoder.new(quality: 12) }
    assert_raises(BrotliSplice::Error) { BrotliSplice::Encoder.new(lgwin: 9) }
    assert_raises(BrotliSplice::Error) { BrotliSplice::Encoder.new(lgwin: 25) }
  end

  def test_uninitialized_and_copied_encoders_are_rejected
    assert_raises(BrotliSplice::Error) { BrotliSplice::Encoder.allocate.write(binary("x")) }

    encoder = BrotliSplice::Encoder.new
    assert_raises(TypeError) { encoder.dup }
    assert_raises(TypeError) { encoder.clone }
    assert_raises(BrotliSplice::Error) { encoder.send(:initialize) }
  end

  def test_streaming_overhead_is_bounded
    prefix = binary(("<div>compressible storefront content</div>" * 1_000))
    slot = binary("placeholder-token".ljust(128, ".") + "\r\n")
    suffix = binary(("<p>more compressible storefront content</p>" * 1_000))
    html = prefix + slot + suffix
    one_shot = BrotliSplice.encode(html, prefix.bytesize, slot.bytesize, quality: 5)

    encoder = BrotliSplice::Encoder.new(quality: 5)
    streaming = encoder.write(prefix.byteslice(0, prefix.bytesize / 2))
    streaming << encoder.write(prefix.byteslice(prefix.bytesize / 2..))
    streaming << encoder.slot(slot)
    streaming << encoder.finish(suffix)

    assert_equal(html, Brotli.inflate(streaming))
    assert_operator(streaming.bytesize - one_shot[:data].bytesize, :<=, 64)
  end

  private

  def binary(value)
    value.b
  end
end
