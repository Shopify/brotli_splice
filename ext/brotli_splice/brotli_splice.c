/*
 * BrotliSplice — Ruby C extension for spliceable Brotli encoding.
 */

#include <ruby.h>
#include <ruby/encoding.h>
#include <brotli/encode.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static VALUE mBrotliSplice;
static VALUE cEncoder;
static VALUE eBrotliSpliceError;

#define CTX_LEN 2
#define OUTPUT_CHUNK_SIZE 16384
static const uint8_t CTX_BYTES[CTX_LEN] = { '\r', '\n' };

typedef enum {
    ENCODER_UNINITIALIZED,
    ENCODER_ACTIVE,
    ENCODER_FINISHED,
    ENCODER_CLOSED,
    ENCODER_FAILED,
} encoder_lifecycle_t;

typedef struct {
    BrotliEncoderState *encoder;
    int quality;
    int lgwin;
    size_t uncompressed_bytes;
    size_t bytes_written;
    size_t slot_offset;
    size_t slot_length;
    size_t native_bytes;
    int has_slot;
    encoder_lifecycle_t lifecycle;
} splice_encoder_t;

typedef union {
    struct {
        size_t allocation_size;
    } tracked;
    max_align_t alignment;
} allocation_header_t;

static void *encoder_alloc_native(void *opaque, size_t size) {
    splice_encoder_t *encoder = opaque;
    if (size > SIZE_MAX - sizeof(allocation_header_t)) return NULL;

    size_t allocation_size = sizeof(allocation_header_t) + size;
    allocation_header_t *header = malloc(allocation_size);
    if (!header) return NULL;
    if (encoder->native_bytes > SIZE_MAX - allocation_size) {
        free(header);
        return NULL;
    }

    header->tracked.allocation_size = allocation_size;
    encoder->native_bytes += allocation_size;
    return header + 1;
}

static void encoder_free_native(void *opaque, void *address) {
    if (!address) return;

    splice_encoder_t *encoder = opaque;
    allocation_header_t *header = ((allocation_header_t *)address) - 1;
    encoder->native_bytes -= header->tracked.allocation_size;
    free(header);
}

static void encoder_destroy_native(splice_encoder_t *encoder) {
    if (!encoder->encoder) return;

    BrotliEncoderDestroyInstance(encoder->encoder);
    encoder->encoder = NULL;
}

static void encoder_terminalize(splice_encoder_t *encoder, encoder_lifecycle_t lifecycle) {
    encoder_destroy_native(encoder);
    encoder->lifecycle = lifecycle;
}

static void encoder_free(void *pointer) {
    splice_encoder_t *encoder = pointer;
    encoder_destroy_native(encoder);
    xfree(encoder);
}

static size_t encoder_memsize(const void *pointer) {
    if (!pointer) return 0;

    const splice_encoder_t *encoder = pointer;
    if (encoder->native_bytes > SIZE_MAX - sizeof(splice_encoder_t)) return SIZE_MAX;
    return sizeof(splice_encoder_t) + encoder->native_bytes;
}

static const rb_data_type_t encoder_type = {
    "BrotliSplice::Encoder",
    {NULL, encoder_free, encoder_memsize},
    NULL,
    NULL,
    RUBY_TYPED_FREE_IMMEDIATELY,
};

static VALUE encoder_alloc(VALUE klass) {
    splice_encoder_t *encoder;
    VALUE object = TypedData_Make_Struct(klass, splice_encoder_t, &encoder_type, encoder);
    memset(encoder, 0, sizeof(*encoder));
    return object;
}

static BrotliEncoderState *new_encoder(splice_encoder_t *encoder, int quality, int lgwin, uint32_t stream_offset) {
    BrotliEncoderState *state = BrotliEncoderCreateInstance(encoder_alloc_native, encoder_free_native, encoder);
    if (!state) rb_raise(eBrotliSpliceError, "encoder creation failed");

    if (!BrotliEncoderSetParameter(state, BROTLI_PARAM_QUALITY, (uint32_t)quality) ||
        !BrotliEncoderSetParameter(state, BROTLI_PARAM_LGWIN, (uint32_t)lgwin) ||
        (stream_offset > 0 && !BrotliEncoderSetParameter(state, BROTLI_PARAM_STREAM_OFFSET, stream_offset))) {
        BrotliEncoderDestroyInstance(state);
        rb_raise(eBrotliSpliceError, "invalid encoder parameters");
    }

    return state;
}

static void check_string(VALUE string) {
    Check_Type(string, T_STRING);
}

static VALUE binary_string(const uint8_t *bytes, size_t length) {
    VALUE result = rb_str_new((const char *)bytes, length);
    rb_enc_associate(result, rb_ascii8bit_encoding());
    return result;
}

static VALUE encoder_feed(BrotliEncoderState *state, BrotliEncoderOperation operation,
                          const uint8_t *input, size_t input_length) {
    VALUE output = rb_str_buf_new(64);
    size_t available_in = input_length;
    const uint8_t *next_in = input;

    for (;;) {
        uint8_t buffer[OUTPUT_CHUNK_SIZE];
        size_t available_out = sizeof(buffer);
        uint8_t *next_out = buffer;
        size_t before_in = available_in;

        if (!BrotliEncoderCompressStream(
                state,
                operation,
                &available_in,
                &next_in,
                &available_out,
                &next_out,
                NULL)) {
            rb_raise(eBrotliSpliceError, "encoding failed");
        }

        size_t produced = sizeof(buffer) - available_out;
        if (produced > 0) rb_str_cat(output, (const char *)buffer, produced);

        if (operation == BROTLI_OPERATION_FINISH) {
            if (BrotliEncoderIsFinished(state)) break;
        } else if (available_in == 0 && !BrotliEncoderHasMoreOutput(state)) {
            break;
        }

        if (before_in == available_in && produced == 0) {
            rb_raise(eBrotliSpliceError, "encoder made no progress");
        }
    }

    rb_enc_associate(output, rb_ascii8bit_encoding());
    return output;
}

static void make_uncompressed_header(uint8_t output[3], size_t length) {
    uint32_t value = (uint32_t)(length - 1);
    uint32_t bits = (value << 3) | (1u << 19);
    output[0] = (uint8_t)bits;
    output[1] = (uint8_t)(bits >> 8);
    output[2] = (uint8_t)(bits >> 16);
}

static VALUE encoder_initialize(int argc, VALUE *argv, VALUE self) {
    VALUE options;
    rb_scan_args(argc, argv, "0:", &options);

    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);

    if (encoder->lifecycle != ENCODER_UNINITIALIZED) {
        rb_raise(eBrotliSpliceError, "encoder is already initialized");
    }

    int quality_value = 11;
    int lgwin_value = 22;
    if (!NIL_P(options)) {
        VALUE quality = rb_hash_aref(options, ID2SYM(rb_intern("quality")));
        VALUE lgwin = rb_hash_aref(options, ID2SYM(rb_intern("lgwin")));
        if (!NIL_P(quality)) quality_value = NUM2INT(quality);
        if (!NIL_P(lgwin)) lgwin_value = NUM2INT(lgwin);
    }
    if (quality_value < 0 || quality_value > 11) {
        rb_raise(eBrotliSpliceError, "quality must be between 0 and 11");
    }
    if (lgwin_value < 10 || lgwin_value > 24) {
        rb_raise(eBrotliSpliceError, "lgwin must be between 10 and 24");
    }
    if (encoder->lifecycle != ENCODER_UNINITIALIZED) {
        rb_raise(eBrotliSpliceError, "encoder is already initialized");
    }

    encoder->quality = quality_value;
    encoder->lgwin = lgwin_value;
    encoder->encoder = new_encoder(encoder, encoder->quality, encoder->lgwin, 0);
    encoder->lifecycle = ENCODER_ACTIVE;
    return self;
}

static void ensure_writable(splice_encoder_t *encoder) {
    switch (encoder->lifecycle) {
        case ENCODER_ACTIVE:
            return;
        case ENCODER_FINISHED:
            rb_raise(eBrotliSpliceError, "encoder is finished");
        case ENCODER_CLOSED:
            rb_raise(eBrotliSpliceError, "encoder is closed");
        case ENCODER_FAILED:
            rb_raise(eBrotliSpliceError, "encoder failed");
        case ENCODER_UNINITIALIZED:
            rb_raise(eBrotliSpliceError, "encoder is not initialized");
    }
}

static VALUE encoder_initialize_copy(VALUE self, VALUE original) {
    rb_raise(rb_eTypeError, "BrotliSplice::Encoder cannot be copied");
}

typedef struct {
    splice_encoder_t *encoder;
    VALUE bytes;
} encoder_operation_t;

static VALUE run_encoder_operation(
    splice_encoder_t *encoder,
    VALUE (*operation)(VALUE),
    encoder_operation_t *arguments
) {
    int state = 0;
    VALUE output = rb_protect(operation, (VALUE)arguments, &state);
    if (state) {
        encoder_terminalize(encoder, ENCODER_FAILED);
        rb_jump_tag(state);
    }
    return output;
}

static VALUE encoder_write_operation(VALUE opaque) {
    encoder_operation_t *arguments = (encoder_operation_t *)opaque;
    splice_encoder_t *encoder = arguments->encoder;
    VALUE bytes = arguments->bytes;
    VALUE output = encoder_feed(
        encoder->encoder,
        BROTLI_OPERATION_FLUSH,
        (const uint8_t *)RSTRING_PTR(bytes),
        (size_t)RSTRING_LEN(bytes));
    encoder->uncompressed_bytes += (size_t)RSTRING_LEN(bytes);
    encoder->bytes_written += (size_t)RSTRING_LEN(output);
    return output;
}

static VALUE encoder_write(VALUE self, VALUE bytes) {
    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);
    ensure_writable(encoder);
    check_string(bytes);

    encoder_operation_t arguments = {encoder, bytes};
    return run_encoder_operation(encoder, encoder_write_operation, &arguments);
}

static VALUE encoder_slot_operation(VALUE opaque) {
    encoder_operation_t *arguments = (encoder_operation_t *)opaque;
    splice_encoder_t *encoder = arguments->encoder;
    VALUE bytes = arguments->bytes;
    size_t total_length = (size_t)RSTRING_LEN(bytes);
    size_t body_length = total_length - CTX_LEN;
    size_t max_stream_offset = ((size_t)1) << encoder->lgwin;
    uint32_t stream_offset = (uint32_t)(
        encoder->uncompressed_bytes + body_length > max_stream_offset
            ? max_stream_offset
            : encoder->uncompressed_bytes + body_length);

    VALUE output = encoder_feed(
        encoder->encoder,
        BROTLI_OPERATION_FLUSH,
        NULL,
        0);
    size_t flushed_length = (size_t)RSTRING_LEN(output);

    BrotliEncoderState *next_encoder = new_encoder(encoder, encoder->quality, encoder->lgwin, stream_offset);
    encoder_destroy_native(encoder);
    encoder->encoder = next_encoder;

    uint8_t header[3];
    make_uncompressed_header(header, body_length);
    rb_str_cat(output, (const char *)header, sizeof(header));
    rb_str_cat(output, RSTRING_PTR(bytes), body_length);

    VALUE context_output = encoder_feed(
        encoder->encoder,
        BROTLI_OPERATION_PROCESS,
        CTX_BYTES,
        CTX_LEN);
    rb_str_concat(output, context_output);

    encoder->slot_offset = encoder->bytes_written + flushed_length + sizeof(header);
    encoder->slot_length = body_length;
    encoder->has_slot = 1;
    encoder->bytes_written += flushed_length + sizeof(header) + body_length + (size_t)RSTRING_LEN(context_output);
    encoder->uncompressed_bytes += body_length + CTX_LEN;
    return output;
}

static VALUE encoder_slot(VALUE self, VALUE bytes) {
    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);
    ensure_writable(encoder);
    check_string(bytes);

    if (encoder->has_slot) rb_raise(eBrotliSpliceError, "slot may only be written once");

    size_t total_length = (size_t)RSTRING_LEN(bytes);
    if (total_length <= CTX_LEN) rb_raise(eBrotliSpliceError, "slot must be longer than 2 bytes");

    size_t body_length = total_length - CTX_LEN;
    if (body_length > 65536) rb_raise(eBrotliSpliceError, "slot body must not exceed 64 KB");
    if (memcmp(RSTRING_PTR(bytes) + body_length, CTX_BYTES, CTX_LEN) != 0) {
        rb_raise(eBrotliSpliceError, "slot must end with the context suffix \\r\\n");
    }

    encoder_operation_t arguments = {encoder, bytes};
    return run_encoder_operation(encoder, encoder_slot_operation, &arguments);
}

static VALUE encoder_finish_operation(VALUE opaque) {
    encoder_operation_t *arguments = (encoder_operation_t *)opaque;
    splice_encoder_t *encoder = arguments->encoder;
    VALUE bytes = arguments->bytes;
    VALUE output = encoder_feed(
        encoder->encoder,
        BROTLI_OPERATION_FINISH,
        (const uint8_t *)RSTRING_PTR(bytes),
        (size_t)RSTRING_LEN(bytes));
    encoder->uncompressed_bytes += (size_t)RSTRING_LEN(bytes);
    encoder->bytes_written += (size_t)RSTRING_LEN(output);
    encoder_terminalize(encoder, ENCODER_FINISHED);
    return output;
}

static VALUE encoder_finish(int argc, VALUE *argv, VALUE self) {
    VALUE bytes;
    rb_scan_args(argc, argv, "01", &bytes);
    if (NIL_P(bytes)) bytes = binary_string(NULL, 0);

    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);
    ensure_writable(encoder);
    check_string(bytes);

    encoder_operation_t arguments = {encoder, bytes};
    return run_encoder_operation(encoder, encoder_finish_operation, &arguments);
}

static VALUE encoder_close(VALUE self) {
    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);
    if (encoder->lifecycle == ENCODER_ACTIVE || encoder->lifecycle == ENCODER_UNINITIALIZED) {
        encoder_terminalize(encoder, ENCODER_CLOSED);
    }
    return Qnil;
}

static VALUE encoder_slot_offset(VALUE self) {
    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);
    return encoder->has_slot ? SIZET2NUM(encoder->slot_offset) : Qnil;
}

static VALUE encoder_slot_length(VALUE self) {
    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);
    return encoder->has_slot ? SIZET2NUM(encoder->slot_length) : Qnil;
}

static VALUE encoder_context_suffix(VALUE self) {
    return binary_string(CTX_BYTES, CTX_LEN);
}

typedef struct {
    VALUE encoder;
    VALUE prefix;
    VALUE slot;
    VALUE suffix;
} encode_operation_t;

static VALUE encode_operation(VALUE opaque) {
    encode_operation_t *arguments = (encode_operation_t *)opaque;
    VALUE data = rb_funcall(arguments->encoder, rb_intern("write"), 1, arguments->prefix);
    rb_str_concat(data, rb_funcall(arguments->encoder, rb_intern("slot"), 1, arguments->slot));
    rb_str_concat(data, rb_funcall(arguments->encoder, rb_intern("finish"), 1, arguments->suffix));

    VALUE result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("data")), data);
    rb_hash_aset(result, ID2SYM(rb_intern("secret_offset")), rb_funcall(arguments->encoder, rb_intern("slot_offset"), 0));
    rb_hash_aset(result, ID2SYM(rb_intern("secret_length")), rb_funcall(arguments->encoder, rb_intern("slot_length"), 0));
    rb_hash_aset(result, ID2SYM(rb_intern("context_suffix")), encoder_context_suffix(arguments->encoder));
    return result;
}

static VALUE close_encoder_ensure(VALUE encoder_value) {
    return encoder_close(encoder_value);
}

static VALUE rb_brotli_splice_encode(int argc, VALUE *argv, VALUE self) {
    VALUE html, secret_offset_value, secret_length_value, options;
    rb_scan_args(argc, argv, "3:", &html, &secret_offset_value, &secret_length_value, &options);
    Check_Type(html, T_STRING);

    size_t secret_offset = NUM2SIZET(secret_offset_value);
    size_t secret_length = NUM2SIZET(secret_length_value);
    size_t html_length = (size_t)RSTRING_LEN(html);
    if (secret_length <= CTX_LEN) rb_raise(eBrotliSpliceError, "secret_length must be > %d", CTX_LEN);
    if (secret_offset > html_length || secret_length > html_length - secret_offset) {
        rb_raise(eBrotliSpliceError, "secret_offset + secret_length exceeds html size");
    }

    VALUE prefix = binary_string((const uint8_t *)RSTRING_PTR(html), secret_offset);
    VALUE slot = binary_string((const uint8_t *)RSTRING_PTR(html) + secret_offset, secret_length);
    VALUE suffix = binary_string(
        (const uint8_t *)RSTRING_PTR(html) + secret_offset + secret_length,
        html_length - secret_offset - secret_length);
    rb_str_modify(slot);
    memcpy(RSTRING_PTR(slot) + secret_length - CTX_LEN, CTX_BYTES, CTX_LEN);

    VALUE encoder = rb_class_new_instance_kw(NIL_P(options) ? 0 : 1, NIL_P(options) ? NULL : &options, cEncoder, RB_PASS_KEYWORDS);
    encode_operation_t arguments = {encoder, prefix, slot, suffix};
    return rb_ensure(encode_operation, (VALUE)&arguments, close_encoder_ensure, encoder);
}

static VALUE rb_brotli_splice_replace(VALUE self, VALUE data, VALUE secret,
                                       VALUE offset_value, VALUE length_value) {
    Check_Type(data, T_STRING);
    Check_Type(secret, T_STRING);
    size_t offset = NUM2SIZET(offset_value);
    size_t length = NUM2SIZET(length_value);

    if ((size_t)RSTRING_LEN(secret) != length) {
        rb_raise(eBrotliSpliceError, "secret length %ld != expected %zu", RSTRING_LEN(secret), length);
    }
    if (offset > (size_t)RSTRING_LEN(data) || length > (size_t)RSTRING_LEN(data) - offset) {
        rb_raise(eBrotliSpliceError, "offset+length exceeds data size");
    }

    VALUE result = rb_str_dup(data);
    rb_str_modify(result);
    memcpy(RSTRING_PTR(result) + offset, RSTRING_PTR(secret), length);
    return result;
}

void Init_brotli_splice(void) {
    mBrotliSplice = rb_define_module("BrotliSplice");
    eBrotliSpliceError = rb_define_class_under(mBrotliSplice, "Error", rb_eRuntimeError);
    cEncoder = rb_define_class_under(mBrotliSplice, "Encoder", rb_cObject);

    rb_define_alloc_func(cEncoder, encoder_alloc);
    rb_define_method(cEncoder, "initialize", encoder_initialize, -1);
    rb_define_method(cEncoder, "initialize_copy", encoder_initialize_copy, 1);
    rb_define_method(cEncoder, "write", encoder_write, 1);
    rb_define_method(cEncoder, "slot", encoder_slot, 1);
    rb_define_method(cEncoder, "finish", encoder_finish, -1);
    rb_define_method(cEncoder, "close", encoder_close, 0);
    rb_define_method(cEncoder, "slot_offset", encoder_slot_offset, 0);
    rb_define_method(cEncoder, "slot_length", encoder_slot_length, 0);
    rb_define_method(cEncoder, "context_suffix", encoder_context_suffix, 0);

    rb_define_singleton_method(mBrotliSplice, "encode", rb_brotli_splice_encode, -1);
    rb_define_singleton_method(mBrotliSplice, "replace", rb_brotli_splice_replace, 4);
}
