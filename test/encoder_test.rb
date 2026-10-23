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

  def test_streaming_slots_are_each_replaceable
    encoder = BrotliSplice::Encoder.new(quality: 5)
    compressed = encoder.write(binary("<head>"))
    compressed << encoder.slot(binary("first-slot\r\n"))
    compressed << encoder.write(binary("<title>Store</title>"))
    compressed << encoder.slot(binary("second-slot\r\n"))
    compressed << encoder.finish(binary("</head>"))

    first, second = encoder.slots
    assert_equal([10, 11], [first[:length], second[:length]])
    assert_equal([second[:offset], second[:length]], [encoder.slot_offset, encoder.slot_length])

    replaced = BrotliSplice.replace(compressed, binary("FIRST-SLOT"), first[:offset], first[:length])
    assert_equal("<head>FIRST-SLOT\r\n<title>Store</title>second-slot\r\n</head>", Brotli.inflate(replaced))
    replaced = BrotliSplice.replace(replaced, binary("SECOND-SLOT"), second[:offset], second[:length])
    assert_equal("<head>FIRST-SLOT\r\n<title>Store</title>SECOND-SLOT\r\n</head>", Brotli.inflate(replaced))
  end

  def test_adjacent_slots_decode_and_replace
    encoder = BrotliSplice::Encoder.new(quality: 5)
    compressed = encoder.slot(binary("aaaa\r\n"))
    compressed << encoder.slot(binary("bbbb\r\n"))
    compressed << encoder.finish(binary("tail"))

    assert_equal("aaaa\r\nbbbb\r\ntail", Brotli.inflate(compressed))
    replacements = encoder.slots.zip(["AAAA", "BBBB"]).map do |slot, value|
      [binary(value), slot[:offset], slot[:length]]
    end
    assert_equal("AAAA\r\nBBBB\r\ntail", Brotli.inflate(BrotliSplice.replace_slots(compressed, replacements)))
  end

  def test_encode_slots_with_one_slot_matches_encode
    html = binary("<html><head>" + ("<p>storefront</p>" * 50) + "placeholder\r\n</head></html>")
    offset = html.index("placeholder")
    length = binary("placeholder\r\n").bytesize

    single = BrotliSplice.encode(html, offset, length, quality: 5)
    multiple = BrotliSplice.encode_slots(html, [[offset, length]], quality: 5)

    assert_equal(single[:data], multiple[:data])
    assert_equal([{ offset: single[:secret_offset], length: single[:secret_length] }], multiple[:slots])
    assert_equal(single[:context_suffix], multiple[:context_suffix])
  end

  def test_encode_slots_and_replace_slots_round_trip
    html = "<title>Café 中文</title><script>var a = {SLOT-A##};</script><div>body</div>" \
      "<meta content=\"SLOT-B##\"><p>end</p><i>SLOT-C##</i>"
    slots = ["SLOT-A##", "SLOT-B##", "SLOT-C##"].map { |placeholder| [html.b.index(placeholder), placeholder.bytesize] }

    result = BrotliSplice.encode_slots(html, slots, quality: 5)

    assert_equal(html.b.gsub("##", "\r\n"), Brotli.inflate(result[:data]))
    replacements = result[:slots].zip(["111111", "222222", "333333"]).map do |slot, value|
      [binary(value), slot[:offset], slot[:length]]
    end
    replaced = BrotliSplice.replace_slots(result[:data], replacements)
    sequential = replacements.reduce(result[:data]) do |data, (secret, offset, length)|
      BrotliSplice.replace(data, secret, offset, length)
    end
    assert_equal(sequential, replaced)
    assert_equal(
      html.b.sub("SLOT-A##", "111111\r\n").sub("SLOT-B##", "222222\r\n").sub("SLOT-C##", "333333\r\n"),
      Brotli.inflate(replaced),
    )
    assert_equal(html.b.gsub("##", "\r\n"), Brotli.inflate(result[:data]))
  end

  def test_slots_past_the_window_decode_and_replace
    random = Random.new(42)
    words = ["product", "collection", "<div class=\"price\">", "</div>", "checkout", "description", " "]
    html = binary("")
    slots = []
    40.times do |index|
      html << binary(Array.new(80) { words.sample(random: random) }.join)
      placeholder = binary("slot-#{index}".ljust(16, ".") + "\r\n")
      slots << [html.bytesize, placeholder.bytesize]
      html << placeholder
    end
    html << binary(Array.new(80) { words.sample(random: random) }.join)

    result = BrotliSplice.encode_slots(html, slots, quality: 5, lgwin: 10)

    assert_operator(html.bytesize, :>, 20 * 1024)
    assert_equal(html, Brotli.inflate(result[:data]))
    expected = html.dup
    replacements = result[:slots].each_with_index.map do |slot, index|
      value = binary("value-#{index}".ljust(slot[:length], "!"))
      expected[slots[index][0], slot[:length]] = value
      [value, slot[:offset], slot[:length]]
    end
    assert_equal(expected, Brotli.inflate(BrotliSplice.replace_slots(result[:data], replacements)))
  end

  def test_encode_slots_without_slots
    result = BrotliSplice.encode_slots(binary("no slots"), [], quality: 5)

    assert_equal("no slots", Brotli.inflate(result[:data]))
    assert_equal([], result[:slots])
  end

  def test_encode_slots_rejects_invalid_slots
    html = binary("aaaa\r\nbbbb\r\ncccc")

    assert_equal(2, BrotliSplice.encode_slots(html, [[0, 6], [6, 6]])[:slots].size)
    assert_raises(BrotliSplice::Error) { BrotliSplice.encode_slots(html, [[6, 6], [0, 6]]) }
    assert_raises(BrotliSplice::Error) { BrotliSplice.encode_slots(html, [[0, 6], [4, 6]]) }
    assert_raises(BrotliSplice::Error) { BrotliSplice.encode_slots(html, [[0, 2]]) }
    assert_raises(BrotliSplice::Error) { BrotliSplice.encode_slots(html, [[12, 6]]) }
    assert_raises(ArgumentError) { BrotliSplice.encode_slots(html, [[0, 6, 1]]) }
    assert_raises(TypeError) { BrotliSplice.encode_slots(html, [0, 6]) }
  end

  def test_encode_slots_rechecks_input_length_after_numeric_coercion
    html = binary("slot\r\n")
    length = Object.new
    length.define_singleton_method(:to_int) do
      html.replace("x")
      6
    end

    assert_raises(BrotliSplice::Error) { BrotliSplice.encode_slots(html, [[0, length]]) }
  end

  def test_replace_slots_rejects_invalid_replacements_without_touching_the_input
    result = BrotliSplice.encode_slots(binary("aaaa\r\nbbbb\r\n"), [[0, 6], [6, 6]], quality: 5)
    first, second = result[:slots]
    data = result[:data].freeze

    assert_raises(BrotliSplice::Error) do
      BrotliSplice.replace_slots(
        data,
        [[binary("AAAA"), first[:offset], first[:length]], [binary("BB"), second[:offset], second[:length]]],
      )
    end
    assert_raises(BrotliSplice::Error) { BrotliSplice.replace_slots(data, [[binary("AAAA"), data.bytesize - 2, 4]]) }
    assert_equal("aaaa\r\nbbbb\r\n", Brotli.inflate(data))
  end

  def test_replace_slots_rechecks_secret_length_after_numeric_coercion
    result = BrotliSplice.encode_slots(binary("aaaa\r\n"), [[0, 6]], quality: 5)
    slot = result[:slots].first
    secret = binary("AAAA")
    offset = Object.new
    offset.define_singleton_method(:to_int) do
      secret.replace("A")
      slot[:offset]
    end

    assert_raises(BrotliSplice::Error) { BrotliSplice.replace_slots(result[:data], [[secret, offset, slot[:length]]]) }
  end

  private

  def binary(value)
    value.b
  end
end
