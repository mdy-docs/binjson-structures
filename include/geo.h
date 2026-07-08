/*
 * geo.h — geographic helpers for the R-tree radius search.
 *
 * C ports of the haversineDistance and radiusToBoundingBox functions in
 * src/rtree.js. These use the WASM libm trig functions (sin/cos/atan2/sqrt);
 * results may differ from JavaScript's Math by a few ULPs, which is acceptable
 * for radius queries.
 */
#ifndef GEO_H
#define GEO_H

#ifdef __cplusplus
extern "C" {
#endif

/* Great-circle distance between two lat/lng points, in kilometres. */
double geo_haversine_distance(double lat1, double lng1, double lat2, double lng2);

/*
 * Convert a radius query (km) around (lat, lng) into a bounding box, writing the
 * corners through the out params. Mirrors src/rtree.js radiusToBoundingBox.
 */
void geo_radius_to_bbox(double lat, double lng, double radius_km,
                        double *min_lat, double *max_lat,
                        double *min_lng, double *max_lng);

#ifdef __cplusplus
}
#endif

#endif /* GEO_H */
