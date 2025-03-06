#ifndef PDF_H
#define PDF_H

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>

// TODO: Maybe I can have another check where you can define your own DS_H
#ifdef PDF_IMPLEMENTATION
#define DS_DA_IMPLEMENTATION
#define DS_SS_IMPLEMENTATION
#define DS_SB_IMPLEMENTATION
#define DS_IO_IMPLEMENTATION
#define DS_AP_IMPLEMENTATION
#endif

#include "ds.h"

#ifndef PDFDEF
#ifdef PDF_STATIC
#define PDFDEF static
#else
#define PDFDEF extern
#endif
#endif

typedef enum filter_kind {
    filter_flate_decode,
    filter_dct_decode
} filter_kind;

typedef enum object_kind {
    object_boolean,
    object_real,
    object_int,
    object_string,
    object_name,
    object_array,
    object_dictionary,
    object_stream,
    object_null,
    object_indirect,
    object_pointer,
} object_kind;

typedef int boolean;

typedef struct indirect_object {
    int object_number;
    int generation_number;
    ds_dynamic_array objects; /* object_t */
} indirect_object;

typedef struct pointer_object {
    int object_number;
    int generation_number;
} pointer_object;

typedef struct object {
    object_kind kind;
    union {
        boolean bool;
        float real;
        int integer;
        char *string;
        char *name;
        ds_dynamic_array array; /* object_t */
        ds_dynamic_array dictionary; /* object_kv */
        ds_string_slice stream;
        indirect_object object;
        pointer_object pointer;
    };
} object_t;

typedef struct object_kv {
    char *name;
    object_t object;
} object_kv;

typedef struct xref_entry {
    int object_number;
    int generation_number;
    char in_use;
} xref_entry;

typedef struct xref {
    int id;
    ds_dynamic_array entries; /* xref_entry */
} xref_t;

typedef struct pdf {
    ds_dynamic_array objects; /* indirect_object */
    xref_t xref;
    ds_dynamic_array trailer; /* object_kv */
    int startxref;
    // TODO: Add the xref table
} pdf_t;

PDFDEF int parse_pdf(char *buffer, int buffer_len, pdf_t *pdf);

#endif // PDF_H

// TODO: Remove
#define PDF_IMPLEMENTATION

#ifdef PDF_IMPLEMENTATION

static void skip_comments(ds_string_slice *slice) {
    ds_string_slice line;
    while (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("%"))) {
        ds_string_slice_tokenize(slice, '\n', &line);
    }
}

static int isnamechar(char c) {
    return isalpha(c) || isdigit(c) || c == '.' || c == '+' || c == '-';
}

static int isnumber(char c) {
    return isdigit(c) || c == '-' || c == '.';
}

static bool ispointer(ds_string_slice *slice) {
    // TODO: we ignore floats for now, also negative numbers

    ds_string_slice token;
    ds_string_slice tmp_slice = *slice;
    ds_string_slice_take_while_pred(&tmp_slice, isnumber, &token);
    ds_string_slice_trim_left_ws(&tmp_slice);
    if (ds_string_slice_starts_with_pred(&tmp_slice, isnumber)) {
        ds_string_slice_take_while_pred(&tmp_slice, isnumber, &token);
        ds_string_slice_trim_left_ws(&tmp_slice);
        if (ds_string_slice_starts_with(&tmp_slice, &DS_STRING_SLICE("R"))) {
            return true;
        } else {
            return false;
        }
    }

    return false;
}

static int parse_direct_object(ds_string_slice *slice, object_t *object);

static int parse_dictionary_object(ds_string_slice *slice, object_t *object) {
    int result = 0;

    object->kind = object_dictionary;
    ds_dynamic_array_init(&object->dictionary, sizeof(object_kv));

    ds_string_slice_trim_left(slice, '<');

    while (1) {
        object_kv obj_kv;

        if (ds_string_slice_empty(slice)) {
            DS_LOG_ERROR("Expected a name or `>>` but found EOF");
            return_defer(1);
        }

        ds_string_slice_trim_left_ws(slice);
        if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE(">>"))) {
            ds_string_slice_trim_left(slice, '>');
            break;
        } else if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("/"))) {
            ds_string_slice token;
            ds_string_slice_step(slice, 1);
            ds_string_slice_take_while_pred(slice, isnamechar, &token);
            ds_string_slice_to_owned(&token, &obj_kv.name);
        } else {
            DS_LOG_ERROR("Expected a name or `>>`");
            return_defer(1);
        }

        if (parse_direct_object(slice, &obj_kv.object) != 0) {
            DS_LOG_ERROR("Could not parse object in dictionary");
            return_defer(1);
        }

        ds_dynamic_array_append(&object->dictionary, &obj_kv);
    }

defer:
    return result;
}

static int parse_string_object(ds_string_slice *slice, object_t *object) {
    int result = 0;
    char start = *slice->str;
    char end = start == '<' ? '>' : ')';

    object->kind = object_string;

    ds_string_slice_step(slice, 1); // Remove the <

    ds_string_slice tmp_slice = *slice;
    tmp_slice.len = 0;

    while (*slice->str != end) {
        if (*slice->str == '\\') {
            ds_string_slice_step(slice, 1);
            tmp_slice.len += 1;
        }

        ds_string_slice_step(slice, 1);
        tmp_slice.len += 1;
    }

    ds_string_slice_to_owned(&tmp_slice, &object->string);
    ds_string_slice_step(slice, 1); // Remove the >

defer:
    return result;
}

static int parse_stream_object(ds_string_slice *slice, object_t *object) {
    char *tmp;
    ds_string_slice token;
    int result = 0;

    object->kind = object_stream;

    ds_string_slice line;
    if (ds_string_slice_tokenize(slice, '\n', &line) != 0) {
        DS_LOG_ERROR("Expected a line but found EOF");
        return_defer(1);
    }

    object->stream = *slice;
    object->stream.len = 0;

    while (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("endstream")) == 0) {
        ds_string_slice_step(slice, 1);
        object->stream.len += 1;
    }

    ds_string_slice_step(slice, strlen("endstream"));

defer:
    return result;
}

static int parse_array_object(ds_string_slice *slice, object_t *object) {
    char *tmp;
    ds_string_slice token;
    int result = 0;

    object->kind = object_array;
    ds_dynamic_array_init(&object->array, sizeof(object_t));

    ds_string_slice_trim_left(slice, '[');

    while (1) {
        object_t obj;

        if (ds_string_slice_empty(slice)) {
            DS_LOG_ERROR("Expected an object or `]` but found EOF");
            return_defer(1);
        }

        ds_string_slice_trim_left_ws(slice);
        if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("]"))) {
            ds_string_slice_trim_left(slice, ']');
            break;
        }

        if (parse_direct_object(slice, &obj) != 0) {
            DS_LOG_ERROR("Could not parse object in dictionary");
            return_defer(1);
        }

        ds_dynamic_array_append(&object->array, &obj);
    }

defer:
    return result;
}

static int parse_name_object(ds_string_slice *slice, object_t *object) {
    char *tmp;
    ds_string_slice token;
    int result = 0;

    object->kind = object_name;

    ds_string_slice_step(slice, 1); // Remove the /

    ds_string_slice_take_while_pred(slice, isnamechar, &token);
    ds_string_slice_to_owned(&token, &tmp);
    object->name = tmp;
    ds_string_slice_trim_left_ws(slice);

defer:
    return result;
}

static int parse_pointer_object(ds_string_slice *slice, object_t *object) {
    char *tmp;
    ds_string_slice token;
    int result = 0;

    object->kind = object_pointer;

    ds_string_slice_take_while_pred(slice, isnumber, &token);
    ds_string_slice_to_owned(&token, &tmp);
    object->pointer.object_number = atoi(tmp);
    free(tmp);
    ds_string_slice_trim_left_ws(slice);

    ds_string_slice_take_while_pred(slice, isnumber, &token);
    ds_string_slice_to_owned(&token, &tmp);
    object->pointer.generation_number = atoi(tmp);
    free(tmp);
    ds_string_slice_trim_left_ws(slice);

    ds_string_slice_step(slice, 1); // Remove the R

defer:
    return result;
}

static int parse_number_object(ds_string_slice *slice, object_t *object) {
    char *tmp;
    ds_string_slice token;
    int result = 0;

    ds_string_slice_take_while_pred(slice, isnumber, &token);
    ds_string_slice_to_owned(&token, &tmp);

    char *tmp2 = tmp;
    int is_int = 1;
    for (char c = *tmp2; c != '\0'; c = *tmp2++) {
        is_int = is_int && (c != '.');
    }

    if (is_int) {
        object->kind = object_int;
        object->integer = atoi(tmp);
    } else {
        object->kind = object_real;
        object->real = atof(tmp);
    }

defer:
    return result;
}

int parse_boolean_object(ds_string_slice *slice, object_t *object) {
    char *tmp;
    ds_string_slice token;
    int result = 0;

    object->kind = object_boolean;

    ds_string_slice_take_while_pred(slice, isnamechar, &token);
    ds_string_slice_to_owned(&token, &tmp);

    object->bool = strncmp(tmp, "true", 4) == 0;

defer:
    return result;
}

static int parse_direct_object(ds_string_slice *slice, object_t *object) {
    int result = 0;

    ds_string_slice_trim_left_ws(slice);
    if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("<<"))) {
        if (parse_dictionary_object(slice, object) != 0) {
            DS_LOG_ERROR("Failed to parse dictionary");
            return_defer(1);
        }
    } else if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("<")) || ds_string_slice_starts_with(slice, &DS_STRING_SLICE("("))) {
        if (parse_string_object(slice, object) != 0) {
            DS_LOG_ERROR("Failed to parse string");
            return_defer(1);
        }
    } else if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("stream"))) {
        if (parse_stream_object(slice, object) != 0) {
            DS_LOG_ERROR("Failed to parse stream");
            return_defer(1);
        }
    } else if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("["))) {
        if (parse_array_object(slice, object) != 0) {
            DS_LOG_ERROR("Failed to parse array");
            return_defer(1);
        }
    } else if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("/"))) {
        if (parse_name_object(slice, object) != 0) {
            DS_LOG_ERROR("Failed to parse name");
            return_defer(1);
        }
    } else {
        if (ds_string_slice_starts_with_pred(slice, isnumber)) {
            if (ispointer(slice)) {
                if (parse_pointer_object(slice, object) != 0) {
                    DS_LOG_ERROR("Failed to parse pointer");
                    return_defer(1);
                }
            } else {
                if (parse_number_object(slice, object) != 0) {
                    DS_LOG_ERROR("Failed to parse number");
                    return_defer(1);
                }
            }
        } else {
            if (parse_boolean_object(slice, object) != 0) {
                DS_LOG_ERROR("Failed to parse boolean");
                return_defer(1);
            }
        }
    }

defer:
    return result;
}

static int parse_indirect_object(ds_string_slice *slice, indirect_object *object) {
    int result = 0;

    ds_string_slice line;
    if (ds_string_slice_tokenize(slice, '\n', &line) != 0) {
        DS_LOG_ERROR("Expected a line but found EOF");
        return_defer(1);
    }

    // we must have `x y obj`
    ds_string_slice token;
    char *word = NULL;
    ds_dynamic_array_init(&object->objects, sizeof(object_t));

    // could use ws stuff
    ds_string_slice_tokenize(&line, ' ', &token);
    ds_string_slice_to_owned(&token, &word);
    object->object_number = atoi(word);
    free(word);

    ds_string_slice_tokenize(&line, ' ', &token);
    ds_string_slice_to_owned(&token, &word);
    object->generation_number = atoi(word);
    free(word);

    // we should have a direct object (or more)
    while (1) {
        if (ds_string_slice_empty(slice)) {
            DS_LOG_ERROR("Expected a direct object or `endobj` keyword but found EOF");
            return_defer(1);
        }

        ds_string_slice_trim_left_ws(slice);
        if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("endobj"))) {
            ds_string_slice_tokenize(slice, '\n', &line);
            break;
        } else {
            object_t obj;
            parse_direct_object(slice, &obj);
            ds_dynamic_array_append(&object->objects, &obj);
        }
    }

defer:
    return result;
}

static int parse_xref(ds_string_slice *slice, xref_t *object) {
    /*
    xref 42 5
    0000001000 65535 f
    0000001234 00000 n
    0000001987 00000 n
    0000011987 00000 n
    0000031987 00000 n
    */

    int result = 0;

    ds_string_slice token;
    char *word = NULL;
    int count = 0;

    ds_dynamic_array_init(&object->entries, sizeof(xref_entry));

    // we must have `xref`
    if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("xref")) == 0) {
        DS_LOG_ERROR("Expected `xref` keyword but found EOF");
        return_defer(1);
    }
    ds_string_slice_step(slice, 4); // Remove the xref

    ds_string_slice_trim_left_ws(slice);
    ds_string_slice_take_while_pred(slice, isnumber, &token);
    ds_string_slice_to_owned(&token, &word);
    object->id = atoi(word);
    free(word);

    ds_string_slice_trim_left_ws(slice);
    ds_string_slice_take_while_pred(slice, isnumber, &token);
    ds_string_slice_to_owned(&token, &word);
    count = atoi(word);
    free(word);

    ds_string_slice_trim_left_ws(slice);
    for (int i = 0; i < count; i++) {
        xref_entry entry = {0};
        ds_string_slice line;
        ds_string_slice_tokenize(slice, '\n', &line);

        ds_string_slice_trim_left_ws(&line);
        ds_string_slice_take_while_pred(&line, isnumber, &token);
        ds_string_slice_to_owned(&token, &word);
        entry.object_number = atoi(word);
        free(word);

        ds_string_slice_trim_left_ws(&line);
        ds_string_slice_take_while_pred(&line, isnumber, &token);
        ds_string_slice_to_owned(&token, &word);
        entry.generation_number = atoi(word);
        free(word);

        ds_string_slice_trim_left_ws(&line);
        entry.in_use = *line.str;

        ds_dynamic_array_append(&object->entries, &entry);
    }

defer:
    return result;
}

static int parse_trailer(ds_string_slice *slice, ds_dynamic_array *trailer) {
    /*
trailer << /Root 5 0 R
           /Size 6
        >>
    */

    int result = 0;

    object_t object = {0};

    // we must have `trailer`
    if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("trailer")) == 0) {
        DS_LOG_ERROR("Expected `trailer` keyword but found EOF");
        return_defer(1);
    }
    ds_string_slice_step(slice, 7); // Remove the trailer

    ds_string_slice_trim_left_ws(slice);

    if (parse_dictionary_object(slice, &object) != 0) {
        DS_LOG_ERROR("Failed to parse dictionary");
        return_defer(1);
    }

    *trailer = object.dictionary;

defer:
    return result;
}

static int parse_startxref(ds_string_slice *slice, int *startxref) {
    int result = 0;

    ds_string_slice token;
    char *word = NULL;

    // we must have `startxref`
    if (ds_string_slice_starts_with(slice, &DS_STRING_SLICE("startxref")) == 0) {
        DS_LOG_ERROR("Expected `startxref` keyword but found EOF");
        return_defer(1);
    }
    ds_string_slice_step(slice, 9); // Remove the startxref

    ds_string_slice_trim_left_ws(slice);
    ds_string_slice_take_while_pred(slice, isnumber, &token);
    ds_string_slice_to_owned(&token, &word);
    *startxref = atoi(word);
    free(word);

defer:
    return result;
}

PDFDEF int parse_pdf(char *buffer, int buffer_len, pdf_t *pdf) {
    int result = 0;
    indirect_object object = {0};
    ds_string_slice slice, line;
    ds_string_slice_init(&slice, buffer, buffer_len);
    ds_dynamic_array_init(&pdf->objects, sizeof(object_t));

    while (1) {
        skip_comments(&slice);
        ds_string_slice_trim_left_ws(&slice);
        if (ds_string_slice_empty(&slice)) {
            break;
        }

        if (ds_string_slice_starts_with(&slice, &DS_STRING_SLICE("xref"))) {
            xref_t xref = {0};
            parse_xref(&slice, &xref);
            pdf->xref = xref;
        } else if (ds_string_slice_starts_with(&slice, &DS_STRING_SLICE("trailer"))) {
            parse_trailer(&slice, &pdf->trailer);
        } else if (ds_string_slice_starts_with(&slice, &DS_STRING_SLICE("startxref"))) {
            parse_startxref(&slice, &pdf->startxref);
        } else if (ds_string_slice_starts_with(&slice, &DS_STRING_SLICE("%%EOF"))) {
            break;
        } else {
            if (parse_indirect_object(&slice, &object) == 0) {
                ds_dynamic_array_append(&pdf->objects, &object);
            }
        }
    }

defer:
    return result;
}

#endif // PDF_IMPLEMENTATION
