#include "catalog_builder.h"
#include <stdlib.h>
#include <string.h>
#include <float.h>

typedef struct {
    int    class_id;
    char   class_name[64];
    int    total_detections;
    int    *track_ids;        /* dynamic array of unique track IDs */
    int    track_count;
    int    track_capacity;
    double first_seen_ms;
    double last_seen_ms;
} ClassAccumulator;

struct CatalogBuilder {
    ClassAccumulator *classes;
    int               num_classes;
    int               total_detections;
};

CatalogBuilder *catalog_builder_create(int num_classes) {
    CatalogBuilder *cb = calloc(1, sizeof(CatalogBuilder));
    if (!cb) return NULL;
    cb->num_classes = num_classes > 0 ? num_classes : 256;
    cb->classes = calloc(cb->num_classes, sizeof(ClassAccumulator));
    for (int i = 0; i < cb->num_classes; i++) {
        cb->classes[i].class_id = i;
        cb->classes[i].first_seen_ms = DBL_MAX;
        cb->classes[i].last_seen_ms  = -1;
        cb->classes[i].track_capacity = 32;
        cb->classes[i].track_ids = malloc(32 * sizeof(int));
    }
    return cb;
}

static bool has_track_id(const ClassAccumulator *acc, int track_id) {
    for (int i = 0; i < acc->track_count; i++) {
        if (acc->track_ids[i] == track_id) return true;
    }
    return false;
}

VPError catalog_builder_add(CatalogBuilder *builder, const DetectionList *detections) {
    if (!builder || !detections) return VP_ERR_INVALID_ARG;

    for (int i = 0; i < detections->count; i++) {
        const Detection *det = &detections->items[i];
        int cid = det->class_id;

        /* Grow class array if needed */
        if (cid >= builder->num_classes) {
            int new_size = cid + 64;
            builder->classes = realloc(builder->classes,
                                        new_size * sizeof(ClassAccumulator));
            for (int j = builder->num_classes; j < new_size; j++) {
                memset(&builder->classes[j], 0, sizeof(ClassAccumulator));
                builder->classes[j].class_id = j;
                builder->classes[j].first_seen_ms = DBL_MAX;
                builder->classes[j].last_seen_ms  = -1;
                builder->classes[j].track_capacity = 32;
                builder->classes[j].track_ids = malloc(32 * sizeof(int));
            }
            builder->num_classes = new_size;
        }

        ClassAccumulator *acc = &builder->classes[cid];
        acc->total_detections++;
        builder->total_detections++;

        if (det->class_name[0] && !acc->class_name[0]) {
            strncpy(acc->class_name, det->class_name, sizeof(acc->class_name) - 1);
        }

        if (det->timestamp_ms < acc->first_seen_ms)
            acc->first_seen_ms = det->timestamp_ms;
        if (det->timestamp_ms > acc->last_seen_ms)
            acc->last_seen_ms = det->timestamp_ms;

        /* Track unique objects */
        if (det->track_id > 0 && !has_track_id(acc, det->track_id)) {
            if (acc->track_count >= acc->track_capacity) {
                acc->track_capacity *= 2;
                acc->track_ids = realloc(acc->track_ids,
                                          acc->track_capacity * sizeof(int));
            }
            acc->track_ids[acc->track_count++] = det->track_id;
        }
    }

    return VP_OK;
}

VPError catalog_builder_build(CatalogBuilder *builder, Catalog *out) {
    if (!builder || !out) return VP_ERR_INVALID_ARG;

    /* Count non-empty classes */
    int count = 0;
    for (int i = 0; i < builder->num_classes; i++) {
        if (builder->classes[i].total_detections > 0) count++;
    }

    out->entries = malloc(count * sizeof(CatalogEntry));
    if (!out->entries) return VP_ERR_OUT_OF_MEMORY;
    out->count    = 0;
    out->capacity = count;

    for (int i = 0; i < builder->num_classes; i++) {
        ClassAccumulator *acc = &builder->classes[i];
        if (acc->total_detections == 0) continue;

        CatalogEntry *e = &out->entries[out->count++];
        e->class_id            = acc->class_id;
        strncpy(e->class_name, acc->class_name, sizeof(e->class_name) - 1);
        e->total_detections    = acc->total_detections;
        e->total_unique_tracks = acc->track_count;
        e->first_seen_ms       = acc->first_seen_ms;
        e->last_seen_ms        = acc->last_seen_ms;
    }

    return VP_OK;
}

int catalog_builder_total_detections(const CatalogBuilder *builder) {
    return builder ? builder->total_detections : 0;
}

void catalog_builder_reset(CatalogBuilder *builder) {
    if (!builder) return;
    for (int i = 0; i < builder->num_classes; i++) {
        builder->classes[i].total_detections = 0;
        builder->classes[i].track_count = 0;
        builder->classes[i].first_seen_ms = DBL_MAX;
        builder->classes[i].last_seen_ms  = -1;
        builder->classes[i].class_name[0] = '\0';
    }
    builder->total_detections = 0;
}

void catalog_builder_destroy(CatalogBuilder *builder) {
    if (!builder) return;
    for (int i = 0; i < builder->num_classes; i++) {
        free(builder->classes[i].track_ids);
    }
    free(builder->classes);
    free(builder);
}

void catalog_free(Catalog *catalog) {
    if (catalog) { free(catalog->entries); catalog->entries = NULL; catalog->count = 0; }
}
