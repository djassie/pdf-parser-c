#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#define PDF_IMPLEMENTATION
#include "pdf.h"
#include "zlib.h"

filter_kind get_filter_kind(ds_dynamic_array dictionary /* object_kv */) {
    for (int i = 0; i < dictionary.count; i++) {
        object_kv kv = {0};
        ds_dynamic_array_get(&dictionary, i, &kv);

        if (strncmp(kv.name, "Filter", 6) == 0) {
            assert(kv.object.kind == object_name);

            if (strncmp(kv.object.name, "FlateDecode", 11) == 0) {
                return filter_flate_decode;
            } else if (strncmp(kv.object.name, "DCTDecode", 9) == 0) {
                return filter_dct_decode;
            }
        }
    }

    return 2;
}

void show_text(ds_string_slice stream, char *filename, indirect_object object) {
    Bytef *source = (Bytef *)(stream.str);
    uLong sourceLen = (uLong)(stream.len);
    uLongf destLen = sourceLen * 8;
    Bytef *dest = calloc(sizeof(Bytef), destLen);
    int result = uncompress(dest, &destLen, source, sourceLen);
    if (result != Z_OK) {
        DS_LOG_ERROR("Failed to uncompress data at : %d", result);
    }

    ds_string_builder string_builder;
    ds_string_builder_init(&string_builder);
    char *text = (char *)dest;
    int text_len = (int)destLen;

    for (int j = 0; j < text_len; j++) {
        if (text[j] == '(') {
            j = j + 1;

            for (; j < text_len; j++) {
                if (text[j] == ')') {
                    break;
                }

                ds_string_builder_appendc(&string_builder, text[j]);
            }
        }
    }

    char *plain_text = NULL;
    result = ds_string_builder_build(&string_builder, &plain_text);
    if (result != 0) {
        DS_LOG_ERROR("Could not extract text");
    }

    ds_string_builder sb = {0};
    ds_string_builder_init(&sb);

    ds_string_builder_append(&sb, "%s_%d_%d.txt", filename, object.object_number, object.generation_number);

    char *path = NULL;
    ds_string_builder_build(&sb, &path);
    ds_io_write(path, plain_text, strlen(plain_text), "w");
}

void show_image(ds_string_slice stream, char *filename, indirect_object object) {
    ds_string_builder sb = {0};
    ds_string_builder_init(&sb);

    ds_string_builder_append(&sb, "%s_%d_%d.jpeg", filename, object.object_number, object.generation_number);

    char *path = NULL;
    ds_string_builder_build(&sb, &path);
    ds_io_write(path, stream.str, stream.len, "wb");
}

int main(int argc, char **argv) {
    int result = 0;
    ds_argparse_parser parser;
    ds_argparse_parser_init(&parser, "pdf-parser", "A simple pdf parser in C", "0.1");

    ds_argparse_add_argument(&parser, (ds_argparse_options){ .short_name = 'i', .long_name = "input", .description = "The input pdf file", .type = ARGUMENT_TYPE_POSITIONAL, .required = 1 });
    ds_argparse_add_argument(&parser, (ds_argparse_options){ .short_name = 'd', .long_name = "directory", .description = "The directory where the pdf file contents are extracted to", .type = ARGUMENT_TYPE_VALUE, .required = 0 });

    if (ds_argparse_parse(&parser, argc, argv) != 0) {
        DS_LOG_ERROR("Failed to parse arguments");
        return_defer(-1);
    }

    char *filename = ds_argparse_get_value(&parser, "input");
    char *directory = ds_argparse_get_value(&parser, "directory");

    char *output_path = NULL;
    ds_string_builder sb = {0};
    ds_string_builder_init(&sb);
    struct stat st = {0};
    if (directory != NULL) {
        if (stat(directory, &st) == -1) {
            mkdir(directory, 0700);
        }

        ds_string_builder_append(&sb, "%s/", directory);
    }

    ds_string_builder_append(&sb, "%s", filename);
    ds_string_builder_build(&sb, &output_path);

    pdf_t pdf = {0};
    char *buffer = NULL;
    int buffer_len = ds_io_read(filename, &buffer, "rb");
    if (buffer_len < 0) {
        DS_LOG_ERROR("Failed to read the file");
        return_defer(-1);
    }

    result = parse_pdf(buffer, buffer_len, &pdf);
    if (result != 0) {
        DS_LOG_ERROR("Failed to parse the buffer: %d", result);
        return_defer(-1);
    }

    printf("startxref: %d\n", pdf.startxref);

    for (int i = 0; i < pdf.trailer.count; i++) {
        object_kv kv = {0};
        ds_dynamic_array_get(&pdf.trailer, i, &kv);
        printf("trailer: %s\n", kv.name);
    }

    for (int i = 0; i < pdf.xref.entries.count; i++) {
        xref_entry entry = {0};
        ds_dynamic_array_get(&pdf.xref.entries, i, &entry);
        printf("xref: %d %d %c\n", entry.object_number, entry.generation_number, entry.in_use);
    }

    for (int i = 0; i < pdf.objects.count; i++) {
        indirect_object object = {0};
        ds_dynamic_array_get(&pdf.objects, i, &object);

        int is_stream = 0;
        for (int j = 0; j < object.objects.count; j++) {
            object_t obj = {0};
            ds_dynamic_array_get(&object.objects, j, &obj);
            if (obj.kind == object_stream) {
                is_stream = 1;
                break;
            }
        }

        if (is_stream == 1) {
            object_t dictionary = {0};
            ds_dynamic_array_get(&object.objects, 0, &dictionary);
            assert(dictionary.kind == object_dictionary);

            object_t stream = {0};
            ds_dynamic_array_get(&object.objects, 1, &stream);
            assert(stream.kind == object_stream);

            filter_kind kind = get_filter_kind(dictionary.dictionary);

            switch (kind) {
            case filter_flate_decode: show_text(stream.stream, output_path, object); break;
            case filter_dct_decode: show_image(stream.stream, output_path, object); break;
            }
        }
    }

defer:
    if (buffer != NULL) {
        free(buffer);
    }
    return result;
}
