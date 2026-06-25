static const uint CLUSTER_TILES_X            = 16u;
static const uint CLUSTER_TILES_Y            = 9u;
static const uint CLUSTER_DEPTH_SLICES       = 24u;
static const uint CLUSTER_TOTAL              = CLUSTER_TILES_X * CLUSTER_TILES_Y * CLUSTER_DEPTH_SLICES;
static const uint CLUSTER_MAX_LIGHTS         = 256u;
static const uint CLUSTER_MAX_LIGHT_LIST_ENTRIES = 65536u;

struct ClusterEntry {
    uint offset;
    uint count;
};
