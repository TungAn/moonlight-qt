using namespace metal;

struct Vertex
{
    float4 position [[ position ]];
    float2 texCoords;
};

struct ShaderParams
{
    float3 matrix[3];
    float3 offsets;
    float bitnessScaleFactor;
    float sharpenStrength;
    float sharpenClamp;
    float sharpenRadius;
    float sharpenEnabled;
    float2 texelSize;
    float colorSaturation;
    float padding;
};

constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);

vertex Vertex vs_draw(constant Vertex *vertices [[ buffer(0) ]], uint id [[ vertex_id ]])
{
    return vertices[id];
}

inline float3 convertYuv(float3 yuv, constant ShaderParams &params)
{
    yuv *= params.bitnessScaleFactor;
    yuv -= params.offsets;

    float3 rgb;
    rgb.r = dot(yuv, params.matrix[0]);
    rgb.g = dot(yuv, params.matrix[1]);
    rgb.b = dot(yuv, params.matrix[2]);
    return rgb;
}

inline float3 sampleBiplanar(texture2d<float> luminancePlane,
                             texture2d<float> chrominancePlane,
                             float2 texCoords,
                             constant ShaderParams &params)
{
    float3 yuv = float3(luminancePlane.sample(s, texCoords).r,
                        chrominancePlane.sample(s, texCoords).rg);
    return convertYuv(yuv, params);
}

inline float3 sampleTriplanar(texture2d<float> luminancePlane,
                              texture2d<float> chrominancePlaneU,
                              texture2d<float> chrominancePlaneV,
                              float2 texCoords,
                              constant ShaderParams &params)
{
    float3 yuv = float3(luminancePlane.sample(s, texCoords).r,
                        chrominancePlaneU.sample(s, texCoords).r,
                        chrominancePlaneV.sample(s, texCoords).r);
    return convertYuv(yuv, params);
}

inline float3 applySharpen(float3 center,
                           float3 north,
                           float3 south,
                           float3 east,
                           float3 west,
                            constant ShaderParams &params)
{
    float3 blur = (center + north + south + east + west) * 0.2f;
    float clampValue = params.sharpenClamp;
    float3 clampVec = float3(clampValue, clampValue, clampValue);
    float3 diff = clamp(center - blur, -clampVec, clampVec);
    float3 sharpened = center + diff * params.sharpenStrength;
    return clamp(sharpened, 0.0f, 1.0f);
}

fragment float4 ps_draw_biplanar(Vertex v [[ stage_in ]],
                                 constant ShaderParams &params [[ buffer(0) ]],
                                 texture2d<float> luminancePlane [[ texture(0) ]],
                                 texture2d<float> chrominancePlane [[ texture(1) ]])
{
    float3 center = sampleBiplanar(luminancePlane, chrominancePlane, v.texCoords, params);

    if (params.sharpenEnabled > 0.5f && params.sharpenStrength > 0.0f && params.sharpenRadius > 0.0f) {
        float2 texel = params.texelSize * params.sharpenRadius;
        float3 east = sampleBiplanar(luminancePlane, chrominancePlane, v.texCoords + float2(texel.x, 0.0f), params);
        float3 west = sampleBiplanar(luminancePlane, chrominancePlane, v.texCoords - float2(texel.x, 0.0f), params);
        float3 north = sampleBiplanar(luminancePlane, chrominancePlane, v.texCoords + float2(0.0f, texel.y), params);
        float3 south = sampleBiplanar(luminancePlane, chrominancePlane, v.texCoords - float2(0.0f, texel.y), params);
        center = applySharpen(center, north, south, east, west, params);
    }

    float luminance = dot(center, float3(0.2126f, 0.7152f, 0.0722f));
    float3 base = float3(luminance);
    center = base + (center - base) * params.colorSaturation;
    center = clamp(center, 0.0f, 1.0f);

    return float4(center, 1.0f);
}

fragment float4 ps_draw_triplanar(Vertex v [[ stage_in ]],
                                  constant ShaderParams &params [[ buffer(0) ]],
                                  texture2d<float> luminancePlane [[ texture(0) ]],
                                  texture2d<float> chrominancePlaneU [[ texture(1) ]],
                                  texture2d<float> chrominancePlaneV [[ texture(2) ]])
{
    float3 center = sampleTriplanar(luminancePlane, chrominancePlaneU, chrominancePlaneV, v.texCoords, params);

    if (params.sharpenEnabled > 0.5f && params.sharpenStrength > 0.0f && params.sharpenRadius > 0.0f) {
        float2 texel = params.texelSize * params.sharpenRadius;
        float3 east = sampleTriplanar(luminancePlane, chrominancePlaneU, chrominancePlaneV, v.texCoords + float2(texel.x, 0.0f), params);
        float3 west = sampleTriplanar(luminancePlane, chrominancePlaneU, chrominancePlaneV, v.texCoords - float2(texel.x, 0.0f), params);
        float3 north = sampleTriplanar(luminancePlane, chrominancePlaneU, chrominancePlaneV, v.texCoords + float2(0.0f, texel.y), params);
        float3 south = sampleTriplanar(luminancePlane, chrominancePlaneU, chrominancePlaneV, v.texCoords - float2(0.0f, texel.y), params);
        center = applySharpen(center, north, south, east, west, params);
    }

    if (params.sharpenEnabled > 0.5f && params.sharpenStrength > 0.0f && params.sharpenRadius > 0.0f) {
        float2 texel = params.texelSize * params.sharpenRadius;
        float3 east = sampleTriplanar(luminancePlane, chrominancePlaneU, chrominancePlaneV, v.texCoords + float2(texel.x, 0.0f), params);
        float3 west = sampleTriplanar(luminancePlane, chrominancePlaneU, chrominancePlaneV, v.texCoords - float2(texel.x, 0.0f), params);
        float3 north = sampleTriplanar(luminancePlane, chrominancePlaneU, chrominancePlaneV, v.texCoords + float2(0.0f, texel.y), params);
        float3 south = sampleTriplanar(luminancePlane, chrominancePlaneU, chrominancePlaneV, v.texCoords - float2(0.0f, texel.y), params);
        center = applySharpen(center, north, south, east, west, params);
    }

    float luminance = dot(center, float3(0.2126f, 0.7152f, 0.0722f));
    float3 base = float3(luminance);
    center = base + (center - base) * params.colorSaturation;
    center = clamp(center, 0.0f, 1.0f);

    return float4(center, 1.0f);
}

fragment float4 ps_draw_rgb(Vertex v [[ stage_in ]],
                            texture2d<float> rgbTexture [[ texture(0) ]])
{
    return rgbTexture.sample(s, v.texCoords);
}
