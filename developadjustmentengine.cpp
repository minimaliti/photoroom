#include "developadjustmentengine.h"

#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QImage>
#include <QByteArray>
#include <QOpenGLFunctions_4_5_Core>
#include <QSurfaceFormat>
#include <QVector>
#include <QMutexLocker>
#include <QDebug>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr float kInv255 = 1.0f / 255.0f;
constexpr int kPreviewMaxDimension = 960;
constexpr int kMaxTextureDimension = 8192;

const char *kComputeShaderSource = R"(#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(rgba32f, binding = 0) uniform readonly image2D inputImage;
layout(rgba32f, binding = 1) uniform writeonly image2D outputImage;

uniform float exposureMultiplier;
uniform float contrastFactor;
uniform float highlights;
uniform float shadows;
uniform float whites;
uniform float blacks;
uniform float saturationFactor;
uniform float vibranceAmount;
uniform float toneCurveHighlights;
uniform float toneCurveLights;
uniform float toneCurveDarks;
uniform float toneCurveShadows;
uniform float hueShift;
uniform float saturationShift;
uniform float luminanceShift;
uniform float vignetteStrength;
uniform float vignetteFalloff;
uniform float clarityStrength;
uniform float widthInv;
uniform float heightInv;
uniform float centerX;
uniform float centerY;
uniform float sharpening;
uniform float noiseReduction;
uniform float grainAmount;
uniform int applyClarity;
uniform int applySharpening;
uniform int applyNoiseReduction;
uniform int applyGrain;
uniform int imageWidth;
uniform int imageHeight;

float clamp01(float v) {
    return clamp(v, 0.0, 1.0);
}

float applyRange(float value, float amount, float weight) {
    if (abs(amount) < 1e-5) return value;
    float influence = amount * weight;
    if (influence > 0.0) {
        return value + (1.0 - value) * influence;
    }
    return value + value * influence;
}

float smoothstep01(float edge0, float edge1, float x) {
    return smoothstep(edge0, edge1, x);
}

float hueToRgb(float p, float q, float t) {
    if (t < 0.0) t += 1.0;
    if (t > 1.0) t -= 1.0;
    if (t < 1.0 / 6.0) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3.0) return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
    return p;
}

vec3 applyHue(vec3 rgb, float hueShiftValue, float saturationShiftValue, float luminanceShiftValue) {
    if (abs(hueShiftValue) < 1e-5 && abs(saturationShiftValue) < 1e-5 && abs(luminanceShiftValue) < 1e-5) {
        return rgb;
    }
    float maxChannel = max(rgb.r, max(rgb.g, rgb.b));
    float minChannel = min(rgb.r, min(rgb.g, rgb.b));
    float chroma = maxChannel - minChannel;
    float luminance = (maxChannel + minChannel) * 0.5;
    float hue = 0.0;
    if (chroma > 1e-5) {
        if (maxChannel == rgb.r) {
            hue = (rgb.g - rgb.b) / chroma;
        } else if (maxChannel == rgb.g) {
            hue = 2.0 + (rgb.b - rgb.r) / chroma;
        } else {
            hue = 4.0 + (rgb.r - rgb.g) / chroma;
        }
        hue /= 6.0;
        if (hue < 0.0) hue += 1.0;
    }
    float saturation = chroma / (1.0 - abs(2.0 * luminance - 1.0) + 1e-5);
    saturation = clamp01(saturation + saturationShiftValue);
    luminance = clamp01(luminance + luminanceShiftValue);
    hue = mod(hue + hueShiftValue, 1.0);
    float q = luminance < 0.5 ? luminance * (1.0 + saturation) : luminance + saturation - luminance * saturation;
    float p = 2.0 * luminance - q;
    return vec3(
        hueToRgb(p, q, hue + 1.0 / 3.0),
        hueToRgb(p, q, hue),
        hueToRgb(p, q, hue - 1.0 / 3.0)
    );
}

void main() {
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inputImage);
    if (coord.x >= size.x || coord.y >= size.y) {
        return;
    }

    vec4 src = imageLoad(inputImage, coord);
    vec3 rgb = src.rgb * exposureMultiplier;
    float luminance = clamp01(dot(rgb, vec3(0.2126, 0.7152, 0.0722)));

    vec3 adjustChannel(vec3 value) {
        vec3 v = value;
        v = (v - vec3(0.5)) * contrastFactor + vec3(0.5);
        float highlightWeight = smoothstep01(0.55, 1.0, luminance);
        float shadowWeight = smoothstep01(0.0, 0.45, 1.0 - luminance);
        float whitesWeight = smoothstep01(0.7, 1.0, luminance);
        float blacksWeight = smoothstep01(0.0, 0.3, 1.0 - luminance);
        v = vec3(applyRange(v.r, highlights, highlightWeight),
                 applyRange(v.g, highlights, highlightWeight),
                 applyRange(v.b, highlights, highlightWeight));
        v = vec3(applyRange(v.r, shadows, shadowWeight),
                 applyRange(v.g, shadows, shadowWeight),
                 applyRange(v.b, shadows, shadowWeight));
        v = vec3(applyRange(v.r, whites, whitesWeight),
                 applyRange(v.g, whites, whitesWeight),
                 applyRange(v.b, whites, whitesWeight));
        v = vec3(applyRange(v.r, blacks, blacksWeight),
                 applyRange(v.g, blacks, blacksWeight),
                 applyRange(v.b, blacks, blacksWeight));
        float toneHighlight = smoothstep01(0.6, 1.0, luminance);
        float toneLight = smoothstep01(0.4, 0.8, luminance);
        float toneDark = smoothstep01(0.2, 0.6, 1.0 - luminance);
        float toneShadow = smoothstep01(0.0, 0.3, 1.0 - luminance);
        v = vec3(applyRange(v.r, toneCurveHighlights, toneHighlight),
                 applyRange(v.g, toneCurveHighlights, toneHighlight),
                 applyRange(v.b, toneCurveHighlights, toneHighlight));
        v = vec3(applyRange(v.r, toneCurveLights, toneLight),
                 applyRange(v.g, toneCurveLights, toneLight),
                 applyRange(v.b, toneCurveLights, toneLight));
        v = vec3(applyRange(v.r, toneCurveDarks, toneDark),
                 applyRange(v.g, toneCurveDarks, toneDark),
                 applyRange(v.b, toneCurveDarks, toneDark));
        v = vec3(applyRange(v.r, toneCurveShadows, toneShadow),
                 applyRange(v.g, toneCurveShadows, toneShadow),
                 applyRange(v.b, toneCurveShadows, toneShadow));
        return clamp(v, vec3(0.0), vec3(1.0));
    }

    rgb = adjustChannel(rgb);

    float midToneInfluence = 1.0 - abs(luminance - 0.5) * 2.0;
    if (applyClarity == 1) {
        float clarityFactor = 1.0 + clarityStrength * midToneInfluence;
        rgb = clamp((rgb - vec3(luminance)) * clarityFactor + vec3(luminance), 0.0, 1.0);
    }

    float maxChannel = max(rgb.r, max(rgb.g, rgb.b));
    float minChannel = min(rgb.r, min(rgb.g, rgb.b));
    float chroma = maxChannel - minChannel;
    float saturationLevel = maxChannel > 0.0 ? chroma / (maxChannel + 1e-5) : 0.0;

    float combinedSaturation = saturationFactor;
    if (vibranceAmount > 1e-5) {
        float vibranceFactor = 1.0 + vibranceAmount * (1.0 - saturationLevel);
        combinedSaturation *= vibranceFactor;
    } else if (vibranceAmount < -1e-5) {
        float vibranceFactor = 1.0 + vibranceAmount * saturationLevel;
        combinedSaturation *= max(0.0, vibranceFactor);
    }
    float newLuminance = clamp01(dot(rgb, vec3(0.2126, 0.7152, 0.0722)));
    rgb = clamp(vec3(newLuminance) + (rgb - vec3(newLuminance)) * combinedSaturation, 0.0, 1.0);

    rgb = applyHue(rgb, hueShift, saturationShift, luminanceShift);

    // Apply sharpening (reads from original image, applies to adjusted rgb)
    if (applySharpening == 1 && sharpening > 0.01) {
        ivec2 leftCoord = ivec2(max(0, coord.x - 1), coord.y);
        ivec2 rightCoord = ivec2(min(imageWidth - 1, coord.x + 1), coord.y);
        ivec2 upCoord = ivec2(coord.x, max(0, coord.y - 1));
        ivec2 downCoord = ivec2(coord.x, min(imageHeight - 1, coord.y + 1));
        
        vec3 centerOrig = imageLoad(inputImage, coord).rgb * exposureMultiplier;
        vec3 leftOrig = imageLoad(inputImage, leftCoord).rgb * exposureMultiplier;
        vec3 rightOrig = imageLoad(inputImage, rightCoord).rgb * exposureMultiplier;
        vec3 upOrig = imageLoad(inputImage, upCoord).rgb * exposureMultiplier;
        vec3 downOrig = imageLoad(inputImage, downCoord).rgb * exposureMultiplier;
        
        float amount = sharpening * 0.5;
        vec3 detail = centerOrig * 4.0 - (leftOrig + rightOrig + upOrig + downOrig);
        rgb = clamp(rgb + detail * amount, 0.0, 1.0);
    }

    // Apply noise reduction
    if (applyNoiseReduction == 1 && noiseReduction > 0.01) {
        float blend = clamp(noiseReduction * 0.4, 0.0, 1.0);
        float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
        rgb = mix(rgb, vec3(lum), blend);
    }

    // Apply grain
    if (applyGrain == 1 && grainAmount > 0.0) {
        uint seed = uint(coord.x) * 1973u + uint(coord.y) * 9277u + 0x7f4a7c15u;
        seed = (seed << 13u) ^ seed;
        uint result = (seed * (seed * seed * 15731u + 789221u) + 1376312589u) & 0x7fffffffu;
        float noise = float(result) / float(0x7fffffffu) - 0.5;
        float delta = noise * grainAmount;
        rgb = clamp(rgb + vec3(delta), 0.0, 1.0);
    }

    // Apply vignette
    float dx = (float(coord.x) - centerX) * widthInv;
    float dy = (float(coord.y) - centerY) * heightInv;
    float distance = sqrt(dx * dx + dy * dy);
    float weight = clamp(distance * vignetteFalloff, 0.0, 1.0);
    float influence = vignetteStrength * weight * weight;
    if (influence >= 0.0) {
        rgb += (vec3(1.0) - rgb) * influence;
    } else {
        rgb += rgb * influence;
    }

    imageStore(outputImage, coord, vec4(clamp(rgb, 0.0, 1.0), src.a));
}
)";

struct RowRange
{
    int start = 0;
    int end = 0;
};

inline float clamp01(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

inline float smoothstep(float edge0, float edge1, float x)
{
    if (edge0 == edge1) {
        return x >= edge1 ? 1.0f : 0.0f;
    }
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

inline float applyRangeAdjustment(float value, float amount, float weight)
{
    if (std::abs(amount) < std::numeric_limits<float>::epsilon()) {
        return value;
    }

    const float influence = amount * weight;
    if (influence > 0.0f) {
        return value + (1.0f - value) * influence;
    }
    return value + value * influence;
}

inline float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

inline float pseudoRandom(int x, int y)
{
    std::uint32_t seed = static_cast<std::uint32_t>(x) * 1973u + static_cast<std::uint32_t>(y) * 9277u + 0x7f4a7c15u;
    seed = (seed << 13U) ^ seed;
    const std::uint32_t result = (seed * (seed * seed * 15731u + 789221u) + 1376312589u) & 0x7fffffffU;
    return static_cast<float>(result) / static_cast<float>(0x7fffffffU);
}

struct AdjustmentPrecompute
{
    float exposureMultiplier = 1.0f;
    float contrastFactor = 1.0f;
    float highlights = 0.0f;
    float shadows = 0.0f;
    float whites = 0.0f;
    float blacks = 0.0f;
    float clarityStrength = 0.0f;
    float saturationFactor = 1.0f;
    float vibranceAmount = 0.0f;
    float toneCurveHighlights = 0.0f;
    float toneCurveLights = 0.0f;
    float toneCurveDarks = 0.0f;
    float toneCurveShadows = 0.0f;
    float hueShift = 0.0f;
    float saturationShift = 0.0f;
    float luminanceShift = 0.0f;
    float sharpening = 0.0f;
    float noiseReduction = 0.0f;
    float vignetteStrength = 0.0f;
    float vignetteFalloff = 1.0f;
    float grainAmount = 0.0f;
    float invWidth = 0.0f;
    float invHeight = 0.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;
};

AdjustmentPrecompute buildPrecompute(const DevelopAdjustments &adjustments, int width, int height)
{
    AdjustmentPrecompute pre;
    pre.exposureMultiplier = std::pow(2.0f, static_cast<float>(adjustments.exposure));

    const float contrast = static_cast<float>(adjustments.contrast) / 100.0f;
    if (contrast >= 0.0f) {
        pre.contrastFactor = 1.0f + contrast * 1.8f;
    } else {
        pre.contrastFactor = 1.0f / (1.0f - contrast * 0.9f);
    }
    pre.contrastFactor = std::max(0.05f, pre.contrastFactor);

    pre.highlights = static_cast<float>(adjustments.highlights) / 100.0f;
    pre.shadows = static_cast<float>(adjustments.shadows) / 100.0f;
    pre.whites = static_cast<float>(adjustments.whites) / 100.0f;
    pre.blacks = static_cast<float>(adjustments.blacks) / 100.0f;

    pre.clarityStrength = static_cast<float>(adjustments.clarity) / 100.0f * 0.5f;
    pre.saturationFactor = 1.0f + static_cast<float>(adjustments.saturation) / 100.0f;
    pre.vibranceAmount = static_cast<float>(adjustments.vibrance) / 100.0f;

    pre.toneCurveHighlights = static_cast<float>(adjustments.toneCurveHighlights) / 100.0f;
    pre.toneCurveLights = static_cast<float>(adjustments.toneCurveLights) / 100.0f;
    pre.toneCurveDarks = static_cast<float>(adjustments.toneCurveDarks) / 100.0f;
    pre.toneCurveShadows = static_cast<float>(adjustments.toneCurveShadows) / 100.0f;

    pre.hueShift = static_cast<float>(adjustments.hueShift) / 360.0f;
    pre.saturationShift = static_cast<float>(adjustments.saturationShift) / 100.0f;
    pre.luminanceShift = static_cast<float>(adjustments.luminanceShift) / 100.0f;

    pre.sharpening = static_cast<float>(adjustments.sharpening) / 100.0f;
    pre.noiseReduction = static_cast<float>(adjustments.noiseReduction) / 100.0f;

    pre.vignetteStrength = static_cast<float>(adjustments.vignette) / 100.0f;
    pre.vignetteFalloff = 1.5f;
    pre.grainAmount = static_cast<float>(adjustments.grain) / 100.0f * 0.03f;

    pre.invWidth = width > 0 ? 1.0f / static_cast<float>(width) : 0.0f;
    pre.invHeight = height > 0 ? 1.0f / static_cast<float>(height) : 0.0f;
    pre.centerX = static_cast<float>(width) * 0.5f;
    pre.centerY = static_cast<float>(height) * 0.5f;

    return pre;
}

inline void applyHueShift(float &r, float &g, float &b, float hueShift, float saturationShift, float luminanceShift)
{
    if (std::abs(hueShift) < 1e-4f && std::abs(saturationShift) < 1e-4f && std::abs(luminanceShift) < 1e-4f) {
        return;
    }

    const float maxChannel = std::max({r, g, b});
    const float minChannel = std::min({r, g, b});
    float chroma = maxChannel - minChannel;
    float luminance = (maxChannel + minChannel) * 0.5f;

    float hue = 0.0f;
    if (chroma > 1e-4f) {
        if (maxChannel == r) {
            hue = (g - b) / chroma;
        } else if (maxChannel == g) {
            hue = 2.0f + (b - r) / chroma;
        } else {
            hue = 4.0f + (r - g) / chroma;
        }
        hue /= 6.0f;
        if (hue < 0.0f) {
            hue += 1.0f;
        }
    }
    float saturation = chroma / (1.0f - std::abs(2.0f * luminance - 1.0f) + 1e-5f);
    saturation = clamp01(saturation + saturationShift);
    luminance = clamp01(luminance + luminanceShift);

    hue = std::fmod(hue + hueShift + 1.0f, 1.0f);

    auto hueToRgb = [](float p, float q, float t) {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
        if (t < 0.5f) return q;
        if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
        return p;
    };

    float q = luminance < 0.5f
                  ? luminance * (1.0f + saturation)
                  : luminance + saturation - luminance * saturation;
    float p = 2.0f * luminance - q;
    r = hueToRgb(p, q, hue + 1.0f / 3.0f);
    g = hueToRgb(p, q, hue);
    b = hueToRgb(p, q, hue - 1.0f / 3.0f);
}

inline void applyVignette(float &r,
                          float &g,
                          float &b,
                          int x,
                          int y,
                          const AdjustmentPrecompute &pre)
{
    if (std::abs(pre.vignetteStrength) < 1e-5f) {
        return;
    }

    const float dx = (static_cast<float>(x) - pre.centerX) * pre.invWidth;
    const float dy = (static_cast<float>(y) - pre.centerY) * pre.invHeight;
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float weight = clamp01((distance * pre.vignetteFalloff));
    const float influence = pre.vignetteStrength * weight * weight;
    if (influence >= 0.0f) {
        r += (1.0f - r) * influence;
        g += (1.0f - g) * influence;
        b += (1.0f - b) * influence;
    } else {
        r += r * influence;
        g += g * influence;
        b += b * influence;
    }
}

inline void applyGrain(float &r,
                       float &g,
                       float &b,
                       int x,
                       int y,
                       const AdjustmentPrecompute &pre)
{
    if (pre.grainAmount <= 0.0f) {
        return;
    }
    const float noise = pseudoRandom(x, y) - 0.5f;
    const float delta = noise * pre.grainAmount;
    r = clamp01(r + delta);
    g = clamp01(g + delta);
    b = clamp01(b + delta);
}

inline void applySharpening(const QImage &source,
                            QImage &target,
                            int x,
                            int y,
                            float &r,
                            float &g,
                            float &b,
                            const AdjustmentPrecompute &pre)
{
    if (pre.sharpening <= 0.01f) {
        return;
    }

    const int width = source.width();
    const int height = source.height();
    const int maxX = width - 1;
    const int maxY = height - 1;
    const int left = std::max(0, x - 1);
    const int right = std::min(maxX, x + 1);
    const int up = std::max(0, y - 1);
    const int down = std::min(maxY, y + 1);

    auto sample = [&](int sx, int sy) -> std::array<float, 3> {
        const uchar *pixel = source.constScanLine(sy) + sx * 4;
        return {pixel[0] * kInv255, pixel[1] * kInv255, pixel[2] * kInv255};
    };

    const auto center = sample(x, y);
    const auto leftSample = sample(left, y);
    const auto rightSample = sample(right, y);
    const auto upSample = sample(x, up);
    const auto downSample = sample(x, down);

    const float amount = pre.sharpening * 0.5f;
    const float detailR = center[0] * 4.0f - (leftSample[0] + rightSample[0] + upSample[0] + downSample[0]);
    const float detailG = center[1] * 4.0f - (leftSample[1] + rightSample[1] + upSample[1] + downSample[1]);
    const float detailB = center[2] * 4.0f - (leftSample[2] + rightSample[2] + upSample[2] + downSample[2]);

    r = clamp01(r + detailR * amount);
    g = clamp01(g + detailG * amount);
    b = clamp01(b + detailB * amount);
}

inline void applyNoiseReduction(float &r, float &g, float &b, const AdjustmentPrecompute &pre)
{
    if (pre.noiseReduction <= 0.01f) {
        return;
    }

    const float blend = clamp01(pre.noiseReduction * 0.4f);
    const float luminance = r * 0.2126f + g * 0.7152f + b * 0.0722f;
    r = lerp(r, luminance, blend);
    g = lerp(g, luminance, blend);
    b = lerp(b, luminance, blend);
}

inline void applyToneCurve(float &channel,
                           float luminance,
                           const AdjustmentPrecompute &pre)
{
    if (std::abs(pre.toneCurveHighlights) > 1e-4f) {
        const float weight = smoothstep(0.6f, 1.0f, luminance);
        channel = applyRangeAdjustment(channel, pre.toneCurveHighlights, weight);
    }
    if (std::abs(pre.toneCurveLights) > 1e-4f) {
        const float weight = smoothstep(0.4f, 0.8f, luminance);
        channel = applyRangeAdjustment(channel, pre.toneCurveLights, weight);
    }
    if (std::abs(pre.toneCurveDarks) > 1e-4f) {
        const float weight = smoothstep(0.2f, 0.6f, 1.0f - luminance);
        channel = applyRangeAdjustment(channel, pre.toneCurveDarks, weight);
    }
    if (std::abs(pre.toneCurveShadows) > 1e-4f) {
        const float weight = smoothstep(0.0f, 0.3f, 1.0f - luminance);
        channel = applyRangeAdjustment(channel, pre.toneCurveShadows, weight);
    }
}

void processRange(const RowRange &range,
                  const QImage &source,
                  QImage &target,
                  const AdjustmentPrecompute &pre,
                  bool isPreview,
                  const std::shared_ptr<DevelopAdjustmentEngine::CancellationToken> &token)
{
    if (token && token->cancelled.load(std::memory_order_relaxed)) {
        return;
    }

    const int width = source.width();

    for (int y = range.start; y < range.end; ++y) {
        if (token && token->cancelled.load(std::memory_order_relaxed)) {
            return;
        }

        const uchar *srcLine = source.constScanLine(y);
        uchar *dstLine = target.scanLine(y);

        for (int x = 0; x < width; ++x) {
            const int idx = x * 4;
            float r = srcLine[idx + 0] * kInv255;
            float g = srcLine[idx + 1] * kInv255;
            float b = srcLine[idx + 2] * kInv255;
            const float a = srcLine[idx + 3] * kInv255;

            r *= pre.exposureMultiplier;
            g *= pre.exposureMultiplier;
            b *= pre.exposureMultiplier;

            float luminance = clamp01(r * 0.2126f + g * 0.7152f + b * 0.0722f);

            auto adjustChannel = [&](float value) -> float {
                float v = value;
                v = (v - 0.5f) * pre.contrastFactor + 0.5f;
                const float highlightWeight = smoothstep(0.55f, 1.0f, luminance);
                const float shadowWeight = smoothstep(0.0f, 0.45f, 1.0f - luminance);
                const float whitesWeight = smoothstep(0.7f, 1.0f, luminance);
                const float blacksWeight = smoothstep(0.0f, 0.3f, 1.0f - luminance);
                v = applyRangeAdjustment(v, pre.highlights, highlightWeight);
                v = applyRangeAdjustment(v, pre.shadows, shadowWeight);
                v = applyRangeAdjustment(v, pre.whites, whitesWeight);
                v = applyRangeAdjustment(v, pre.blacks, blacksWeight);
                applyToneCurve(v, luminance, pre);
                return clamp01(v);
            };

            r = adjustChannel(r);
            g = adjustChannel(g);
            b = adjustChannel(b);

            const float midToneInfluence = 1.0f - std::abs(luminance - 0.5f) * 2.0f;
            const float clarityFactor = 1.0f + pre.clarityStrength * midToneInfluence;
            r = clamp01((r - luminance) * clarityFactor + luminance);
            g = clamp01((g - luminance) * clarityFactor + luminance);
            b = clamp01((b - luminance) * clarityFactor + luminance);

            float maxChannel = std::max({r, g, b});
            float minChannel = std::min({r, g, b});
            float chroma = maxChannel - minChannel;
            float saturationLevel = maxChannel > 0.0f ? chroma / (maxChannel + 1e-5f) : 0.0f;

            float combinedSaturation = pre.saturationFactor;
            if (pre.vibranceAmount > 1e-5f) {
                const float vibranceFactor = 1.0f + pre.vibranceAmount * (1.0f - saturationLevel);
                combinedSaturation *= vibranceFactor;
            } else if (pre.vibranceAmount < -1e-5f) {
                const float vibranceFactor = 1.0f + pre.vibranceAmount * (saturationLevel);
                combinedSaturation *= std::max(0.0f, vibranceFactor);
            }
            const float newLuminance = clamp01(r * 0.2126f + g * 0.7152f + b * 0.0722f);
            r = clamp01(newLuminance + (r - newLuminance) * combinedSaturation);
            g = clamp01(newLuminance + (g - newLuminance) * combinedSaturation);
            b = clamp01(newLuminance + (b - newLuminance) * combinedSaturation);

            applyHueShift(r, g, b, pre.hueShift, pre.saturationShift, pre.luminanceShift);
            applyVignette(r, g, b, x, y, pre);
            applyGrain(r, g, b, x, y, pre);
            if (!isPreview) {
                applySharpening(source, target, x, y, r, g, b, pre);
                applyNoiseReduction(r, g, b, pre);
            }

            dstLine[idx + 0] = static_cast<uchar>(clamp01(r) * 255.0f + 0.5f);
            dstLine[idx + 1] = static_cast<uchar>(clamp01(g) * 255.0f + 0.5f);
            dstLine[idx + 2] = static_cast<uchar>(clamp01(b) * 255.0f + 0.5f);
            dstLine[idx + 3] = static_cast<uchar>(clamp01(a) * 255.0f + 0.5f);
        }
    }
}

DevelopAdjustmentRenderResult renderImageCpu(const QImage &source,
                                             const DevelopAdjustments &adjustments,
                                             bool isPreview,
                                             const std::shared_ptr<DevelopAdjustmentEngine::CancellationToken> &token)
{
    DevelopAdjustmentRenderResult result;
    if (source.isNull()) {
        result.cancelled = true;
        return result;
    }

    QImage working = source;
    if (working.format() != QImage::Format_RGBA8888 && working.format() != QImage::Format_RGBA8888_Premultiplied) {
        working = working.convertToFormat(QImage::Format_RGBA8888);
    } else if (working.format() == QImage::Format_RGBA8888_Premultiplied) {
        working = working.convertToFormat(QImage::Format_RGBA8888);
    }

    QImage target(working.size(), QImage::Format_RGBA8888);
    target.detach();

    const int height = working.height();
    const int width = working.width();

    const AdjustmentPrecompute pre = buildPrecompute(adjustments, width, height);

    const int maxThreads = std::max(1, QThreadPool::globalInstance()->maxThreadCount());
    const int minRowsPerChunk = 32;
    const int chunkRows = std::max(minRowsPerChunk, (height + maxThreads - 1) / maxThreads);

    std::vector<RowRange> ranges;
    ranges.reserve((height + chunkRows - 1) / chunkRows);
    for (int start = 0; start < height; start += chunkRows) {
        ranges.push_back({start, std::min(height, start + chunkRows)});
    }

    QElapsedTimer timer;
    timer.start();

    QtConcurrent::blockingMap(ranges, [&](const RowRange &range) {
        processRange(range, working, target, pre, isPreview, token);
    });

    result.elapsedMs = timer.elapsed();

    if (token && token->cancelled.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return result;
    }

    result.image = target;
    return result;
}

} // namespace

DevelopAdjustmentEngine::DevelopAdjustmentEngine(QObject *parent)
    : QObject(parent)
{
}

DevelopAdjustmentEngine::~DevelopAdjustmentEngine()
{
    cancelActive();
    if (m_computeProgram != 0 && m_glContext) {
        QMutexLocker locker(&m_glMutex);
        m_glContext->makeCurrent(m_offscreenSurface.get());
        QOpenGLFunctions_4_5_Core funcs;
        funcs.initializeOpenGLFunctions();
        funcs.glDeleteProgram(m_computeProgram);
        m_glContext->doneCurrent();
    }
}

std::shared_ptr<DevelopAdjustmentEngine::CancellationToken> DevelopAdjustmentEngine::makeActiveToken()
{
    auto token = std::make_shared<CancellationToken>();
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_activeToken) {
        m_activeToken->cancelled.store(true, std::memory_order_release);
    }
    m_activeToken = token;
    return token;
}

void DevelopAdjustmentEngine::cancelActive()
{
    std::lock_guard<std::mutex> guard(m_mutex);
    if (m_activeToken) {
        m_activeToken->cancelled.store(true, std::memory_order_release);
        m_activeToken.reset();
    }
}

QFuture<DevelopAdjustmentRenderResult> DevelopAdjustmentEngine::startRender(
    DevelopAdjustmentRequest request,
    const std::shared_ptr<CancellationToken> &token)
{
    return QtConcurrent::run([this, request = std::move(request), token]() mutable {
        DevelopAdjustmentRenderResult result;
        if (initializeGpu()) {
            result = renderWithGpu(request, token);
        } else {
            result = renderWithCpu(request, token);
        }
        result.requestId = request.requestId;
        result.isPreview = request.isPreview;
        result.displayScale = request.displayScale;
        return result;
    });
}

QFuture<DevelopAdjustmentRenderResult> DevelopAdjustmentEngine::renderAsync(DevelopAdjustmentRequest request)
{
    auto token = makeActiveToken();
    return startRender(std::move(request), token);
}

bool DevelopAdjustmentEngine::initializeGpu()
{
    if (m_gpuInitialized) {
        return m_gpuAvailable;
    }

    m_gpuInitialized = true;

    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setVersion(4, 3); // Minimum for compute shaders
    format.setOption(QSurfaceFormat::DebugContext, false);

    auto context = std::make_unique<QOpenGLContext>();
    context->setFormat(format);
    if (!context->create()) {
        m_gpuAvailable = false;
        return false;
    }

    auto surface = std::make_unique<QOffscreenSurface>();
    surface->setFormat(format);
    surface->create();

    if (!surface->isValid()) {
        m_gpuAvailable = false;
        return false;
    }

    if (!context->makeCurrent(surface.get())) {
        m_gpuAvailable = false;
        return false;
    }

    QOpenGLFunctions_4_5_Core funcs;
    if (!funcs.initializeOpenGLFunctions()) {
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }

    // Check for compute shader support
    GLint maxComputeWorkGroupInvocations = 0;
    funcs.glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxComputeWorkGroupInvocations);
    if (maxComputeWorkGroupInvocations == 0) {
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }

    GLuint shader = funcs.glCreateShader(GL_COMPUTE_SHADER);
    if (shader == 0) {
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }

    funcs.glShaderSource(shader, 1, &kComputeShaderSource, nullptr);
    funcs.glCompileShader(shader);
    
    GLint compileStatus = 0;
    funcs.glGetShaderiv(shader, GL_COMPILE_STATUS, &compileStatus);
    if (!compileStatus) {
        GLint logLength = 0;
        funcs.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        if (logLength > 0) {
            QVector<char> log(logLength);
            funcs.glGetShaderInfoLog(shader, logLength, nullptr, log.data());
            qWarning("Compute shader compilation failed: %s", log.data());
        }
        funcs.glDeleteShader(shader);
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }

    GLuint program = funcs.glCreateProgram();
    if (program == 0) {
        funcs.glDeleteShader(shader);
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }

    funcs.glAttachShader(program, shader);
    funcs.glLinkProgram(program);
    
    GLint linkStatus = 0;
    funcs.glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
    funcs.glDeleteShader(shader);
    
    if (!linkStatus) {
        GLint logLength = 0;
        funcs.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logLength);
        if (logLength > 0) {
            QVector<char> log(logLength);
            funcs.glGetProgramInfoLog(program, logLength, nullptr, log.data());
            qWarning("Compute shader program linking failed: %s", log.data());
        }
        funcs.glDeleteProgram(program);
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }

    m_computeProgram = program;
    m_glContext = std::move(context);
    m_offscreenSurface = std::move(surface);
    m_glContext->doneCurrent();
    m_gpuAvailable = true;
    return true;
}

DevelopAdjustmentRenderResult DevelopAdjustmentEngine::renderWithGpu(const DevelopAdjustmentRequest &request,
                                                                     const std::shared_ptr<CancellationToken> &token)
{
    DevelopAdjustmentRenderResult result;
    result.requestId = request.requestId;
    result.isPreview = request.isPreview;
    result.displayScale = request.displayScale;

    if (token && token->cancelled.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return result;
    }

    if (!m_glContext || !m_offscreenSurface || m_computeProgram == 0) {
        result = renderWithCpu(request, token);
        return result;
    }

    QMutexLocker locker(&m_glMutex);
    if (!m_glContext->makeCurrent(m_offscreenSurface.get())) {
        result = renderWithCpu(request, token);
        return result;
    }

    QElapsedTimer timer;
    timer.start();

    QOpenGLFunctions_4_5_Core funcs;
    funcs.initializeOpenGLFunctions();

    const QImage sourceImage = request.image.convertToFormat(QImage::Format_RGBA8888);
    const int width = sourceImage.width();
    const int height = sourceImage.height();

    if (width <= 0 || height <= 0 ||
        width > kMaxTextureDimension || height > kMaxTextureDimension) {
        m_glContext->doneCurrent();
        result = renderWithCpu(request, token);
        return result;
    }

    // Optimize texture data conversion using constBits for direct access
    QVector<float> textureData(width * height * 4);
    const uchar *srcBits = sourceImage.constBits();
    const int pixelCount = width * height;
    for (int i = 0; i < pixelCount; ++i) {
        const int idx = i * 4;
        textureData[idx + 0] = srcBits[idx + 0] * kInv255;
        textureData[idx + 1] = srcBits[idx + 1] * kInv255;
        textureData[idx + 2] = srcBits[idx + 2] * kInv255;
        textureData[idx + 3] = srcBits[idx + 3] * kInv255;
    }

    GLuint inputTex = 0;
    GLuint outputTex = 0;
    funcs.glGenTextures(1, &inputTex);
    funcs.glGenTextures(1, &outputTex);

    auto releaseTextures = [&]() {
        funcs.glDeleteTextures(1, &inputTex);
        funcs.glDeleteTextures(1, &outputTex);
    };

    // Setup input texture
    funcs.glActiveTexture(GL_TEXTURE0);
    funcs.glBindTexture(GL_TEXTURE_2D, inputTex);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    funcs.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, width, height);
    funcs.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, textureData.constData());

    // Setup output texture
    funcs.glBindTexture(GL_TEXTURE_2D, outputTex);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    funcs.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, width, height);

    funcs.glUseProgram(m_computeProgram);

    auto uniform = [&](const char *name, float value) {
        GLint loc = funcs.glGetUniformLocation(m_computeProgram, name);
        if (loc >= 0) {
            funcs.glUniform1f(loc, value);
        }
    };
    auto uniformInt = [&](const char *name, int value) {
        GLint loc = funcs.glGetUniformLocation(m_computeProgram, name);
        if (loc >= 0) {
            funcs.glUniform1i(loc, value);
        }
    };

    const AdjustmentPrecompute pre = buildPrecompute(request.adjustments,
                                                     width,
                                                     height);

    uniform("exposureMultiplier", pre.exposureMultiplier);
    uniform("contrastFactor", pre.contrastFactor);
    uniform("highlights", pre.highlights);
    uniform("shadows", pre.shadows);
    uniform("whites", pre.whites);
    uniform("blacks", pre.blacks);
    uniform("saturationFactor", pre.saturationFactor);
    uniform("vibranceAmount", pre.vibranceAmount);
    uniform("toneCurveHighlights", pre.toneCurveHighlights);
    uniform("toneCurveLights", pre.toneCurveLights);
    uniform("toneCurveDarks", pre.toneCurveDarks);
    uniform("toneCurveShadows", pre.toneCurveShadows);
    uniform("hueShift", pre.hueShift);
    uniform("saturationShift", pre.saturationShift);
    uniform("luminanceShift", pre.luminanceShift);
    uniform("vignetteStrength", pre.vignetteStrength);
    uniform("vignetteFalloff", pre.vignetteFalloff);
    uniform("clarityStrength", pre.clarityStrength);
    uniform("sharpening", pre.sharpening);
    uniform("noiseReduction", pre.noiseReduction);
    uniform("grainAmount", pre.grainAmount);
    uniform("widthInv", pre.invWidth);
    uniform("heightInv", pre.invHeight);
    uniform("centerX", pre.centerX);
    uniform("centerY", pre.centerY);
    uniformInt("applyClarity", request.isPreview ? 0 : 1);
    uniformInt("applySharpening", (request.isPreview || pre.sharpening <= 0.01f) ? 0 : 1);
    uniformInt("applyNoiseReduction", (request.isPreview || pre.noiseReduction <= 0.01f) ? 0 : 1);
    uniformInt("applyGrain", (pre.grainAmount <= 0.0f) ? 0 : 1);
    uniformInt("imageWidth", width);
    uniformInt("imageHeight", height);

    funcs.glBindImageTexture(0, inputTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    funcs.glBindImageTexture(1, outputTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);

    const GLuint groupsX = (width + 15) / 16;
    const GLuint groupsY = (height + 15) / 16;
    funcs.glDispatchCompute(groupsX, groupsY, 1);

    funcs.glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    if (token && token->cancelled.load(std::memory_order_relaxed)) {
        releaseTextures();
        funcs.glUseProgram(0);
        m_glContext->doneCurrent();
        result.cancelled = true;
        return result;
    }

    // Read back results
    QVector<float> outputData(width * height * 4);
    funcs.glBindTexture(GL_TEXTURE_2D, outputTex);
    funcs.glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, outputData.data());

    releaseTextures();
    funcs.glUseProgram(0);
    m_glContext->doneCurrent();

    // Convert back to QImage
    QImage outputImage(width, height, QImage::Format_RGBA8888);
    outputImage.detach();
    uchar *dstBits = outputImage.bits();
    for (int i = 0; i < pixelCount; ++i) {
        const int idx = i * 4;
        dstBits[idx + 0] = static_cast<uchar>(std::clamp(outputData[idx + 0], 0.0f, 1.0f) * 255.0f + 0.5f);
        dstBits[idx + 1] = static_cast<uchar>(std::clamp(outputData[idx + 1], 0.0f, 1.0f) * 255.0f + 0.5f);
        dstBits[idx + 2] = static_cast<uchar>(std::clamp(outputData[idx + 2], 0.0f, 1.0f) * 255.0f + 0.5f);
        dstBits[idx + 3] = static_cast<uchar>(std::clamp(outputData[idx + 3], 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    result.elapsedMs = timer.elapsed();
    result.image = outputImage;
    return result;
}

DevelopAdjustmentRenderResult DevelopAdjustmentEngine::renderWithCpu(const DevelopAdjustmentRequest &request,
                                                                     const std::shared_ptr<CancellationToken> &token)
{
    return renderImageCpu(request.image, request.adjustments, request.isPreview, token);
}


