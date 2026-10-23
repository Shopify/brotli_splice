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
    size_t offset;
    size_t length;
} slot_position_t;

typedef struct {
    BrotliEncoderState *encoder;
    int quality;
    int lgwin;
    size_t uncompressed_bytes;
    size_t bytes_written;
    slot_position_t *slots;
    size_t slot_count;
    size_t slot_capacity;
    size_t native_bytes;
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
    xfree(encoder->slots);
    xfree(encoder);
}

static size_t encoder_memsize(const void *pointer) {
    if (!pointer) return 0;

    const splice_encoder_t *encoder = pointer;
    size_t fixed_size = sizeof(splice_encoder_t) + encoder->slot_capacity * sizeof(slot_position_t);
    if (encoder->native_bytes > SIZE_MAX - fixed_size) return SIZE_MAX;
    return fixed_size + encoder->native_bytes;
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

    encoder->slots[encoder->slot_count].offset = encoder->bytes_written + flushed_length + sizeof(header);
    encoder->slots[encoder->slot_count].length = body_length;
    encoder->slot_count++;
    encoder->bytes_written += flushed_length + sizeof(header) + body_length + (size_t)RSTRING_LEN(context_output);
    encoder->uncompressed_bytes += body_length + CTX_LEN;
    return output;
}

/*
 * Grows the slot list before any stream bytes are produced, so a failed
 * allocation leaves the encoder usable and the stream unchanged.
 */
static void reserve_slot(splice_encoder_t *encoder) {
    if (encoder->slot_count < encoder->slot_capacity) return;

    size_t capacity = encoder->slot_capacity ? encoder->slot_capacity * 2 : 4;
    REALLOC_N(encoder->slots, slot_position_t, capacity);
    encoder->slot_capacity = capacity;
}

static VALUE encoder_slot(VALUE self, VALUE bytes) {
    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);
    ensure_writable(encoder);
    check_string(bytes);

    size_t total_length = (size_t)RSTRING_LEN(bytes);
    if (total_length <= CTX_LEN) rb_raise(eBrotliSpliceError, "slot must be longer than 2 bytes");

    size_t body_length = total_length - CTX_LEN;
    if (body_length > 65536) rb_raise(eBrotliSpliceError, "slot body must not exceed 64 KB");
    if (memcmp(RSTRING_PTR(bytes) + body_length, CTX_BYTES, CTX_LEN) != 0) {
        rb_raise(eBrotliSpliceError, "slot must end with the context suffix \\r\\n");
    }

    reserve_slot(encoder);

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

static const slot_position_t *last_slot(const splice_encoder_t *encoder) {
    return encoder->slot_count ? &encoder->slots[encoder->slot_count - 1] : NULL;
}

static VALUE encoder_slot_offset(VALUE self) {
    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);
    const slot_position_t *slot = last_slot(encoder);
    return slot ? SIZET2NUM(slot->offset) : Qnil;
}

static VALUE encoder_slot_length(VALUE self) {
    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);
    const slot_position_t *slot = last_slot(encoder);
    return slot ? SIZET2NUM(slot->length) : Qnil;
}

static VALUE encoder_slots(VALUE self) {
    splice_encoder_t *encoder;
    TypedData_Get_Struct(self, splice_encoder_t, &encoder_type, encoder);

    VALUE slots = rb_ary_new_capa((long)encoder->slot_count);
    for (size_t index = 0; index < encoder->slot_count; index++) {
        VALUE slot = rb_hash_new();
        rb_hash_aset(slot, ID2SYM(rb_intern("offset")), SIZET2NUM(encoder->slots[index].offset));
        rb_hash_aset(slot, ID2SYM(rb_intern("length")), SIZET2NUM(encoder->slots[index].length));
        rb_ary_push(slots, slot);
    }
    return slots;
}

static VALUE encoder_context_suffix(VALUE self) {
    return binary_string(CTX_BYTES, CTX_LEN);
}

typedef struct {
    VALUE encoder;
    VALUE pieces;
} encode_operation_t;

/*
 * Pieces alternate between compressed chunks and slots, starting and ending
 * with a compressed chunk: [chunk, slot, chunk, slot, ..., chunk].
 */
static VALUE encode_operation(VALUE opaque) {
    encode_operation_t *arguments = (encode_operation_t *)opaque;
    long last = RARRAY_LEN(arguments->pieces) - 1;
    VALUE data = binary_string(NULL, 0);
    for (long index = 0; index < last; index += 2) {
        rb_str_concat(data, rb_funcall(arguments->encoder, rb_intern("write"), 1, RARRAY_AREF(arguments->pieces, index)));
        rb_str_concat(data, rb_funcall(arguments->encoder, rb_intern("slot"), 1, RARRAY_AREF(arguments->pieces, index + 1)));
    }
    rb_str_concat(data, rb_funcall(arguments->encoder, rb_intern("finish"), 1, RARRAY_AREF(arguments->pieces, last)));
    return data;
}

static VALUE close_encoder_ensure(VALUE encoder_value) {
    return encoder_close(encoder_value);
}

static VALUE new_encoder_with_options(VALUE options) {
    return rb_class_new_instance_kw(NIL_P(options) ? 0 : 1, NIL_P(options) ? NULL : &options, cEncoder, RB_PASS_KEYWORDS);
}

static VALUE encode_pieces(VALUE encoder, VALUE pieces) {
    encode_operation_t arguments = {encoder, pieces};
    return rb_ensure(encode_operation, (VALUE)&arguments, close_encoder_ensure, encoder);
}

/*
 * `bounds` holds validated, ascending, non-overlapping [offset, length] pairs.
 * Each slot copy ends in the context suffix, which `Encoder#slot` requires.
 */
static VALUE slot_pieces(VALUE html, const size_t *bounds, long count) {
    VALUE pieces = rb_ary_new_capa(count * 2 + 1);
    size_t position = 0;
    for (long index = 0; index < count; index++) {
        size_t offset = bounds[index * 2];
        size_t length = bounds[index * 2 + 1];
        rb_ary_push(pieces, binary_string((const uint8_t *)RSTRING_PTR(html) + position, offset - position));

        VALUE slot = binary_string((const uint8_t *)RSTRING_PTR(html) + offset, length);
        rb_str_modify(slot);
        memcpy(RSTRING_PTR(slot) + length - CTX_LEN, CTX_BYTES, CTX_LEN);
        rb_ary_push(pieces, slot);
        position = offset + length;
    }
    rb_ary_push(pieces, binary_string((const uint8_t *)RSTRING_PTR(html) + position, (size_t)RSTRING_LEN(html) - position));
    return pieces;
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

    size_t bounds[2] = {secret_offset, secret_length};
    VALUE pieces = slot_pieces(html, bounds, 1);
    VALUE encoder = new_encoder_with_options(options);
    VALUE data = encode_pieces(encoder, pieces);

    VALUE result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("data")), data);
    rb_hash_aset(result, ID2SYM(rb_intern("secret_offset")), encoder_slot_offset(encoder));
    rb_hash_aset(result, ID2SYM(rb_intern("secret_length")), encoder_slot_length(encoder));
    rb_hash_aset(result, ID2SYM(rb_intern("context_suffix")), encoder_context_suffix(encoder));
    return result;
}

static VALUE rb_brotli_splice_encode_slots(int argc, VALUE *argv, VALUE self) {
    VALUE html, slots, options;
    rb_scan_args(argc, argv, "2:", &html, &slots, &options);
    Check_Type(html, T_STRING);
    Check_Type(slots, T_ARRAY);

    /*
     * Coerce every pair before validating any of them: numeric coercion can run
     * Ruby code that changes `html`.
     */
    long count = RARRAY_LEN(slots);
    VALUE bounds_buffer;
    size_t *bounds = ALLOCV_N(size_t, bounds_buffer, (size_t)count * 2);
    for (long index = 0; index < count; index++) {
        VALUE slot = rb_ary_entry(slots, index);
        Check_Type(slot, T_ARRAY);
        if (RARRAY_LEN(slot) != 2) rb_raise(rb_eArgError, "each slot must be an [offset, length] pair");
        bounds[index * 2] = NUM2SIZET(rb_ary_entry(slot, 0));
        bounds[index * 2 + 1] = NUM2SIZET(rb_ary_entry(slot, 1));
    }

    size_t html_length = (size_t)RSTRING_LEN(html);
    size_t previous_end = 0;
    for (long index = 0; index < count; index++) {
        size_t offset = bounds[index * 2];
        size_t length = bounds[index * 2 + 1];
        if (length <= CTX_LEN) rb_raise(eBrotliSpliceError, "slot length must be > %d", CTX_LEN);
        if (offset > html_length || length > html_length - offset) {
            rb_raise(eBrotliSpliceError, "slot offset + length exceeds html size");
        }
        if (offset < previous_end) rb_raise(eBrotliSpliceError, "slots must be in ascending order and must not overlap");
        previous_end = offset + length;
    }

    VALUE pieces = slot_pieces(html, bounds, count);
    ALLOCV_END(bounds_buffer);
    VALUE encoder = new_encoder_with_options(options);
    VALUE data = encode_pieces(encoder, pieces);

    VALUE result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("data")), data);
    rb_hash_aset(result, ID2SYM(rb_intern("slots")), encoder_slots(encoder));
    rb_hash_aset(result, ID2SYM(rb_intern("context_suffix")), encoder_context_suffix(encoder));
    return result;
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

static VALUE rb_brotli_splice_replace_slots(VALUE self, VALUE data, VALUE replacements) {
    Check_Type(data, T_STRING);
    Check_Type(replacements, T_ARRAY);

    /*
     * Coerce every entry before validating any of them: numeric coercion can run
     * Ruby code that changes `data` or a secret.
     */
    long count = RARRAY_LEN(replacements);
    VALUE secrets = rb_ary_new_capa(count);
    VALUE bounds_buffer;
    size_t *bounds = ALLOCV_N(size_t, bounds_buffer, (size_t)count * 2);
    for (long index = 0; index < count; index++) {
        VALUE replacement = rb_ary_entry(replacements, index);
        Check_Type(replacement, T_ARRAY);
        if (RARRAY_LEN(replacement) != 3) {
            rb_raise(rb_eArgError, "each replacement must be a [secret, offset, length] triple");
        }
        VALUE secret = rb_ary_entry(replacement, 0);
        Check_Type(secret, T_STRING);
        rb_ary_push(secrets, secret);
        bounds[index * 2] = NUM2SIZET(rb_ary_entry(replacement, 1));
        bounds[index * 2 + 1] = NUM2SIZET(rb_ary_entry(replacement, 2));
    }

    size_t data_length = (size_t)RSTRING_LEN(data);
    for (long index = 0; index < count; index++) {
        VALUE secret = RARRAY_AREF(secrets, index);
        size_t offset = bounds[index * 2];
        size_t length = bounds[index * 2 + 1];
        if ((size_t)RSTRING_LEN(secret) != length) {
            rb_raise(eBrotliSpliceError, "secret length %ld != expected %zu", RSTRING_LEN(secret), length);
        }
        if (offset > data_length || length > data_length - offset) {
            rb_raise(eBrotliSpliceError, "offset+length exceeds data size");
        }
    }

    VALUE result = rb_str_dup(data);
    rb_str_modify(result);
    for (long index = 0; index < count; index++) {
        VALUE secret = RARRAY_AREF(secrets, index);
        memcpy(RSTRING_PTR(result) + bounds[index * 2], RSTRING_PTR(secret), bounds[index * 2 + 1]);
    }
    ALLOCV_END(bounds_buffer);
    RB_GC_GUARD(secrets);
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
    rb_define_method(cEncoder, "slots", encoder_slots, 0);
    rb_define_method(cEncoder, "context_suffix", encoder_context_suffix, 0);

    rb_define_singleton_method(mBrotliSplice, "encode", rb_brotli_splice_encode, -1);
    rb_define_singleton_method(mBrotliSplice, "encode_slots", rb_brotli_splice_encode_slots, -1);
    rb_define_singleton_method(mBrotliSplice, "replace", rb_brotli_splice_replace, 4);
    rb_define_singleton_method(mBrotliSplice, "replace_slots", rb_brotli_splice_replace_slots, 2);
}
