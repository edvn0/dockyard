static const float lod_thresholds_sq[5] = {
    0.12f * 0.12f,   // LOD0 → LOD1
    0.07f * 0.07f,   // LOD1 → LOD2
    0.04f * 0.04f,   // LOD2 → LOD3
    0.02f * 0.02f,   // LOD3 → LOD4
    0.008f * 0.008f, // LOD4 → LOD5
};

uint select_lod(float radius, float dist_sq, uint lod_count) {
    if (lod_count <= 1u || dist_sq < 1e-6f)
        return 0u;

    float ratio_sq = (radius * radius) / dist_sq;
    uint result = lod_count - 1u;

    for (uint i = 0; i < lod_count - 1u; ++i) {
        if (ratio_sq >= lod_thresholds_sq[i] && result == lod_count - 1u)
            result = i;
    }

    return result;
}

struct CullResult {
    bool visible;
    float dist_sq;
    uint lod_idx;
};

CullResult cull_instance(
    float4x4 model_matrix,
    float local_radius,
    uint lod_count,
    Array<float4, 6> frustum_planes,
    float3 camera_pos,
    float max_dist_sq)
{
    float3 position = float3(model_matrix[0][3], model_matrix[1][3], model_matrix[2][3]);
    float scale_x = length(float3(model_matrix[0][0], model_matrix[1][0], model_matrix[2][0]));
    float scale_y = length(float3(model_matrix[0][1], model_matrix[1][1], model_matrix[2][1]));
    float scale_z = length(float3(model_matrix[0][2], model_matrix[1][2], model_matrix[2][2]));
    float world_radius = local_radius * max(scale_x, max(scale_y, scale_z));

    bool inside = true;

    [ForceUnroll]
    for (int i = 0; i < 6; ++i)
        inside = inside && (dot(float4(position, 1.0f), frustum_planes[i]) >= -world_radius);

    if (!inside)
        return { false, 0.0f, 0u };

    float3 diff = position - camera_pos;
    float dist_sq = dot(diff, diff);
    if (dist_sq > max_dist_sq)
        return { false, 0.0f, 0u };

    return { true, dist_sq, select_lod(local_radius, dist_sq, lod_count) };
}
