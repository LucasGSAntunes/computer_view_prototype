#ifndef VP_CATALOG_BUILDER_H
#define VP_CATALOG_BUILDER_H

#include "core/common/types.h"
#include "core/common/errors.h"

typedef struct CatalogBuilder CatalogBuilder;

/* Create catalog builder. num_classes = expected number of classes. */
CatalogBuilder *catalog_builder_create(int num_classes);

/* Add detections from a frame to the catalog. */
VPError catalog_builder_add(CatalogBuilder *builder, const DetectionList *detections);

/* Build final catalog summary. Caller must free with catalog_free(). */
VPError catalog_builder_build(CatalogBuilder *builder, Catalog *out);

/* Get total detections added across all frames. */
int catalog_builder_total_detections(const CatalogBuilder *builder);

void catalog_builder_reset(CatalogBuilder *builder);
void catalog_builder_destroy(CatalogBuilder *builder);

void catalog_free(Catalog *catalog);

#endif /* VP_CATALOG_BUILDER_H */
