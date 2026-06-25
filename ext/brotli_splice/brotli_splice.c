/*
 * BrotliSplice — Ruby C extension for spliceable Brotli encoding.
 *
 * Produces a Brotli stream with a fixed-length uncompressed slot whose
 * raw bytes can be overwritten without re-encoding the rest.
 *
 * Uses the standard Brotli streaming API with BROTLI_PARAM_STREAM_OFFSET.
 */

#include <ruby.h>
#include <ruby/encoding.h>
#include <brotli/encode.h>
#include <brotli/decode.h>
#include <string.h>

static VALUE mBrotliSplice;
static VALUE eBrotliSpliceError;

#define CTX_LEN 2
static const uint8_t CTX_BYTES[CTX_LEN] = { '\r', '\n' };

/* ── Streaming encoder helper ─────────────────────────────────── */
static size_t encoder_feed(BrotliEncoderState *s, BrotliEncoderOperation op,
                           const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t out_cap) {
    size_t avail_in = in_len, avail_out = out_cap, total = 0;
    const uint8_t *next_in = in;
    uint8_t *next_out = out;
    if (!BrotliEncoderCompressStream(s, op,
            &avail_in, &next_in, &avail_out, &next_out, &total)) {
        return 0;
    }
    return out_cap - avail_out;
}

/* ── Build uncompressed meta-block (ISLAST=0) ─────────────────── */
static size_t make_uncompressed_block(uint8_t *out,
                                       const uint8_t *data, size_t len) {
    if (len == 0 || len > 65536) return 0;
    uint32_t r = (uint32_t)(len - 1);
    uint32_t nibbles = 0;
    if (len > (1u << 16)) nibbles = (len > (1u << 20)) ? 2 : 1;
    uint32_t bits = (nibbles << 1) | (r << 3) | (1u << (19 + 4 * nibbles));
    out[0] = (uint8_t)bits;
    out[1] = (uint8_t)(bits >> 8);
    out[2] = (uint8_t)(bits >> 16);
    size_t hdr = 3;
    if (nibbles == 2) { out[3] = (uint8_t)(bits >> 24); hdr = 4; }
    memcpy(out + hdr, data, len);
    return hdr + len;
}

/*
 * call-seq:
 *   BrotliSplice.encode(html, secret_offset, secret_length, quality: 11) -> Hash
 *
 * Brotli-encode +html+ with a spliceable slot at the given position.
 *
 * The slot's last 2 bytes are reserved as a fixed context suffix ("\r\n")
 * and cannot be replaced. The replaceable portion is +secret_length - 2+
 * bytes.
 *
 * Returns:
 *   { data:           String,   # the Brotli-encoded stream
 *     secret_offset:  Integer,  # byte offset of the replaceable data in the stream
 *     secret_length:  Integer,  # replaceable byte count (secret_length - 2)
 *     context_suffix: String }  # the 2 fixed context bytes ("\r\n")
 */
static VALUE rb_brotli_splice_encode(int argc, VALUE *argv, VALUE self) {
    VALUE rb_html, rb_sec_off, rb_sec_len, rb_opts;
    rb_scan_args(argc, argv, "3:", &rb_html, &rb_sec_off, &rb_sec_len, &rb_opts);

    Check_Type(rb_html, T_STRING);
    const uint8_t *html = (const uint8_t *)RSTRING_PTR(rb_html);
    size_t html_len = RSTRING_LEN(rb_html);
    size_t sec_off = NUM2SIZET(rb_sec_off);
    size_t sec_total = NUM2SIZET(rb_sec_len);

    if (sec_total <= CTX_LEN)
        rb_raise(eBrotliSpliceError, "secret_length must be > %d", CTX_LEN);
    if (sec_off + sec_total > html_len)
        rb_raise(eBrotliSpliceError, "secret_offset + secret_length exceeds html size");

    int quality = 11;
    if (!NIL_P(rb_opts)) {
        VALUE q = rb_hash_aref(rb_opts, ID2SYM(rb_intern("quality")));
        if (!NIL_P(q)) quality = NUM2INT(q);
    }

    /* Split into part1 / secret_body / context / part3 */
    const uint8_t *part1 = html;
    size_t p1_len = sec_off;

    size_t sec_body = sec_total - CTX_LEN;
    const uint8_t *secret = html + sec_off;          /* secret_body bytes */

    const uint8_t *part3 = html + sec_off + sec_total;
    size_t p3_len = html_len - sec_off - sec_total;

    /* Allocate output buffer */
    size_t out_cap = html_len + 65536;
    uint8_t *out = xmalloc(out_cap);
    size_t pos = 0;

    /* ── Chunk 1: compress part1 with FLUSH ──────────────────────── */
    BrotliEncoderState *enc1 = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (!enc1) { xfree(out); rb_raise(eBrotliSpliceError, "encoder creation failed"); }
    BrotliEncoderSetParameter(enc1, BROTLI_PARAM_QUALITY, quality);
    BrotliEncoderSetParameter(enc1, BROTLI_PARAM_LGWIN, 22);

    size_t c1 = encoder_feed(enc1, BROTLI_OPERATION_FLUSH,
                              part1, p1_len, out + pos, out_cap - pos);
    BrotliEncoderDestroyInstance(enc1);
    if (c1 == 0 && p1_len > 0) {
        xfree(out);
        rb_raise(eBrotliSpliceError, "chunk1 encoding failed");
    }
    pos += c1;

    /* ── Chunk 2: uncompressed meta-block for secret body ────────── */
    size_t sec_data_offset = pos + 3;  /* 3-byte header for ≤64KB */
    size_t c2 = make_uncompressed_block(out + pos, secret, sec_body);
    if (c2 == 0) { xfree(out); rb_raise(eBrotliSpliceError, "chunk2 failed"); }
    pos += c2;

    /* ── Chunk 3: context bytes + part3, STREAM_OFFSET ───────────── */
    BrotliEncoderState *enc2 = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (!enc2) { xfree(out); rb_raise(eBrotliSpliceError, "encoder2 creation failed"); }
    BrotliEncoderSetParameter(enc2, BROTLI_PARAM_QUALITY, quality);
    BrotliEncoderSetParameter(enc2, BROTLI_PARAM_LGWIN, 22);
    BrotliEncoderSetParameter(enc2, BROTLI_PARAM_STREAM_OFFSET,
                               (uint32_t)(p1_len + sec_body));

    size_t c3_in_len = CTX_LEN + p3_len;
    uint8_t *c3_in = xmalloc(c3_in_len);
    memcpy(c3_in, CTX_BYTES, CTX_LEN);
    memcpy(c3_in + CTX_LEN, part3, p3_len);

    size_t c3 = encoder_feed(enc2, BROTLI_OPERATION_FINISH,
                              c3_in, c3_in_len, out + pos, out_cap - pos);
    BrotliEncoderDestroyInstance(enc2);
    xfree(c3_in);
    if (c3 == 0) { xfree(out); rb_raise(eBrotliSpliceError, "chunk3 encoding failed"); }
    pos += c3;

    /* Build result */
    VALUE data = rb_str_new((char *)out, pos);
    rb_enc_associate(data, rb_ascii8bit_encoding());
    xfree(out);

    VALUE result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("data")), data);
    rb_hash_aset(result, ID2SYM(rb_intern("secret_offset")), SIZET2NUM(sec_data_offset));
    rb_hash_aset(result, ID2SYM(rb_intern("secret_length")), SIZET2NUM(sec_body));
    rb_hash_aset(result, ID2SYM(rb_intern("context_suffix")),
                 rb_str_new((const char *)CTX_BYTES, CTX_LEN));
    return result;
}

/*
 * call-seq:
 *   BrotliSplice.replace(compressed_data, new_secret, secret_offset, secret_length) -> String
 *
 * Replace the secret in a compressed stream. +new_secret+ must be
 * exactly +secret_length+ bytes. Returns a new string.
 */
static VALUE rb_brotli_splice_replace(VALUE self,
                                       VALUE rb_data, VALUE rb_secret,
                                       VALUE rb_offset, VALUE rb_length) {
    Check_Type(rb_data, T_STRING);
    Check_Type(rb_secret, T_STRING);
    size_t offset = NUM2SIZET(rb_offset);
    size_t length = NUM2SIZET(rb_length);

    if ((size_t)RSTRING_LEN(rb_secret) != length)
        rb_raise(eBrotliSpliceError,
                 "secret length %ld != expected %zu",
                 RSTRING_LEN(rb_secret), length);

    if (offset + length > (size_t)RSTRING_LEN(rb_data))
        rb_raise(eBrotliSpliceError, "offset+length exceeds data size");

    VALUE result = rb_str_dup(rb_data);
    rb_str_modify(result);
    memcpy(RSTRING_PTR(result) + offset, RSTRING_PTR(rb_secret), length);
    return result;
}

void Init_brotli_splice(void) {
    mBrotliSplice = rb_define_module("BrotliSplice");
    eBrotliSpliceError = rb_define_class_under(mBrotliSplice, "Error", rb_eRuntimeError);

    rb_define_singleton_method(mBrotliSplice, "encode",
                               rb_brotli_splice_encode, -1);
    rb_define_singleton_method(mBrotliSplice, "replace",
                               rb_brotli_splice_replace, 4);
}
