/*
 * geo.c — C port of the haversine / radius-to-bbox math in src/rtree.js.
 * See geo.h. The formulas match the reference exactly; only the underlying
 * libm trig results may differ by a few ULPs from JavaScript's Math.
 */
#include "geo.h"

#include <math.h>

/* The double nearest to pi — identical to JavaScript's Math.PI. */
#define GEO_PI 3.141592653589793

double geo_haversine_distance(double lat1, double lng1, double lat2, double lng2) {
    const double R = 6371.0; /* Earth's radius in kilometres */
    double d_lat = (lat2 - lat1) * GEO_PI / 180.0;
    double d_lng = (lng2 - lng1) * GEO_PI / 180.0;
    double a = sin(d_lat / 2.0) * sin(d_lat / 2.0) +
               cos(lat1 * GEO_PI / 180.0) * cos(lat2 * GEO_PI / 180.0) *
               sin(d_lng / 2.0) * sin(d_lng / 2.0);
    double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
    return R * c;
}

void geo_radius_to_bbox(double lat, double lng, double radius_km,
                        double *min_lat, double *max_lat,
                        double *min_lng, double *max_lng) {
    double lat_delta = radius_km / 111.0;
    double lng_delta = radius_km / (111.0 * cos(lat * GEO_PI / 180.0));
    *min_lat = lat - lat_delta;
    *max_lat = lat + lat_delta;
    *min_lng = lng - lng_delta;
    *max_lng = lng + lng_delta;
}
