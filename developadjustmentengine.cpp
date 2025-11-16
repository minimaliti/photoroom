#include "developadjustmentengine.h"

#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QThread>
#include <QCoreApplication>
#include <QImage>
#include <QByteArray>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_4_3_Core>
#include <QOpenGLFunctions_4_5_Core>
#include <QSurfaceFormat>
#include <QOpenGLContext>
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
#include <memory>
#include <unordered_map>

namespace {

// Performance constants
constexpr float kInv255 = 1.0f / 255.0f;
constexpr float k255 = 255.0f;
constexpr int kPreviewMaxDimension = 960;
constexpr int kMaxTextureDimension = 16384;  // Increased for modern GPUs
constexpr int kWorkgroupSize = 16;  // 16x16 = 256 threads per workgroup (optimal for most GPUs)

// Color science constants (Rec. 709)
constexpr float kLumaR = 0.2126f;
constexpr float kLumaG = 0.7152f;
constexpr float kLumaB = 0.0722f;

// Optimized compute shader with improved algorithms and performance
// Split into multiple parts due to MSVC string literal length limit (16KB)
const char *kComputeShaderSource = R"(#version 430 core
layout(local_size_x = 16, local_size_y = 16) in;

// Use texture binding for better performance on some GPUs
layout(rgba32f, binding = 0) uniform readonly image2D inputImage;
layout(rgba32f, binding = 1) uniform writeonly image2D outputImage;

// Adjustment parameters grouped for better cache coherency
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

// Color science constants (Rec. 709)
const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);
const float kEpsilon = 1e-7;

// ============================================================================
// Utility Functions
// ============================================================================

// Fast clamp to [0, 1] range
float clamp01(float v) {
    return clamp(v, 0.0, 1.0);
}

vec3 clamp01(vec3 v) {
    return clamp(v, vec3(0.0), vec3(1.0));
}

// Optimized smoothstep with early exit
float smoothstep01(float edge0, float edge1, float x) {
    if (x <= edge0) return 0.0;
    if (x >= edge1) return 1.0;
    float t = (x - edge0) / (edge1 - edge0);
    return t * t * (3.0 - 2.0 * t);
}

// Calculate luminance using Rec. 709 weights
float getLuminance(vec3 rgb) {
    return dot(rgb, kLumaWeights);
}

// Whites adjustment - controls the very brightest points (similar to highlight recovery but for extreme highlights)
vec3 applyWhitesAdjust(vec3 value, float amount, float weight) {
    if (abs(amount) < kEpsilon || weight < kEpsilon) return value;
    
    float influence = amount * weight;
    vec3 recovered = value;
    
    if (amount < 0.0) {
        // Lower whites: compress the extreme highlight range to reveal detail
        // Use a strong shoulder curve for the very brightest values (> 0.7)
        vec3 whitesAmount = smoothstep(vec3(0.7), vec3(1.0), value);
        vec3 compression = vec3(abs(influence)) * whitesAmount;
        
        // Strong compression for extreme highlights
        vec3 shoulderGamma = vec3(1.0) + compression * 2.0;
        
        // Only affect values in whites range
        vec3 normalized = clamp01((value - vec3(0.7)) / vec3(0.3));  // Normalize [0.7, 1.0] to [0, 1]
        vec3 compressed = pow(normalized, shoulderGamma);
        
        // Remap compressed values
        vec3 shoulderEnd = vec3(0.7) + vec3(0.3) / shoulderGamma;
        vec3 whitesRecovered = vec3(0.7) + compressed * (shoulderEnd - vec3(0.7));
        
        // Blend based on whites mask
        vec3 blendMask = whitesAmount * vec3(abs(influence) * weight);
        vec3 whitesRange = step(vec3(0.7), value);
        recovered = mix(value, whitesRecovered, blendMask * whitesRange);
    } else {
        // Raise whites: expand toward pure white while preserving detail
        vec3 whitesAmount = smoothstep(vec3(0.7), vec3(1.0), value);
        vec3 expansion = vec3(influence) * whitesAmount;
        recovered = value + (vec3(1.0) - value) * expansion;
    }
    
    return clamp01(recovered);
}

// Blacks adjustment - controls the very darkest points (similar to shadow recovery but for extreme shadows)
vec3 applyBlacksAdjust(vec3 value, float amount, float weight) {
    if (abs(amount) < kEpsilon || weight < kEpsilon) return value;
    
    float influence = amount * weight;
    vec3 recovered = value;
    
    if (amount > 0.0) {
        // Raise blacks: lift the extreme shadow range to reveal detail
        // Use a strong toe curve for the very darkest values (< 0.3)
        vec3 blacksAmount = smoothstep(vec3(0.0), vec3(0.3), vec3(1.0) - value);
        vec3 expansion = vec3(influence) * blacksAmount;
        
        // Strong expansion for extreme shadows
        vec3 toeGamma = vec3(1.0) + expansion * 1.5;
        
        // Only affect values in blacks range
        vec3 normalized = clamp01(value / vec3(0.3));  // Normalize [0, 0.3] to [0, 1]
        vec3 expanded = pow(normalized, vec3(1.0) / toeGamma);
        
        // Calculate lift amount
        vec3 liftAmount = vec3(0.3) * expansion * 0.6;
        vec3 blacksRecovered = expanded * (vec3(0.3) - liftAmount) + liftAmount;
        
        // Blend based on blacks mask
        vec3 blendMask = blacksAmount * vec3(influence * weight);
        vec3 blacksRange = vec3(1.0) - step(vec3(0.3), value);
        recovered = mix(value, blacksRecovered, blendMask * blacksRange);
    } else {
        // Lower blacks: compress toward pure black while preserving detail
        vec3 blacksAmount = smoothstep(vec3(0.0), vec3(0.3), vec3(1.0) - value);
        vec3 compression = vec3(abs(influence)) * blacksAmount;
        
        // Use power curve for compression
        vec3 compressGamma = vec3(1.0) + compression * 1.5;
        vec3 blacksRange = vec3(1.0) - step(vec3(0.3), value);
        vec3 compressed = pow(clamp01(value / vec3(0.3)), compressGamma) * vec3(0.3);
        recovered = mix(value, compressed, blacksAmount * vec3(abs(influence) * weight) * blacksRange);
    }
    
    return clamp01(recovered);
}

// Apply tone range adjustment with optimized blending (for tone curve adjustments)
// Used for Lights and Darks adjustments - preserves detail while adjusting tone
float applyRange(float value, float amount, float weight) {
    if (abs(amount) < kEpsilon || weight < kEpsilon) return value;
    float influence = amount * weight;
    
    // Use curves that preserve detail
    if (influence > 0.0) {
        // Brighten: gentle expansion that preserves highlights
        float expansion = influence * 0.7;
        // Scale up values with more effect on mid-tones, less on extremes
        float brightened = value * (1.0 + expansion) + (1.0 - value) * expansion * 0.4;
        return clamp01(brightened);
    } else {
        // Darken: gentle compression that preserves shadows
        float compression = abs(influence) * 0.5;
        // Use a power curve with limited compression to preserve detail
        float compressionGamma = 1.0 + compression;
        // Ensure gamma doesn't get too high to avoid crushing
        compressionGamma = min(compressionGamma, 2.0);
        float compressed = pow(clamp01(value), compressionGamma);
        // Mix with original to preserve detail
        return clamp01(mix(value, compressed, abs(influence) * weight * 0.8));
    }
}

// ============================================================================
// Tone Adjustment Functions
// ============================================================================

// Highlight recovery using proper tone curve - recovers detail in overexposed areas
vec3 applyHighlightAdjust(vec3 value, float amount, float weight, float luminance) {
    if (abs(amount) < kEpsilon || weight < kEpsilon) return value;
    
    // Calculate highlight mask - stronger for bright areas
    float highlightMask = smoothstep01(0.40, 0.90, luminance);
    float influence = amount * weight * highlightMask;
    
    if (amount < 0.0) {
        // Recover highlights: compress highlight range to reveal detail
        // Use a shoulder curve that compresses highlights while preserving mid-tones
        
        // Determine highlight amount for each channel (component-wise smoothstep)
        vec3 highlightAmount = smoothstep(vec3(0.5), vec3(1.0), value);
        
        // Calculate compression per channel
        vec3 compression = vec3(abs(influence)) * highlightAmount;
        
        // Calculate shoulder gamma for compression (gamma > 1 compresses)
        vec3 shoulderGamma = vec3(1.0) + compression * 1.5;
        
        // Apply highlight recovery curve per channel
        // Only affect values above 0.5 (highlight range)
        vec3 recovered = value;
        
        // Calculate shoulder end point for each channel
        vec3 shoulderEnd = vec3(0.5) + vec3(0.5) / shoulderGamma;
        
        // Normalize highlight range [0.5, 1.0] to [0, 1]
        vec3 normalized = (value - vec3(0.5)) / vec3(0.5);
        
        // Apply compression only to highlight portion
        vec3 compressed = pow(clamp01(normalized), shoulderGamma);
        
        // Remap compressed values back to [0.5, shoulderEnd]
        vec3 highlightRecovered = vec3(0.5) + compressed * (shoulderEnd - vec3(0.5));
        
        // Mix original with recovered based on highlight mask and compression amount
        vec3 blendMask = highlightAmount * vec3(abs(influence) * highlightMask);
        recovered = mix(value, highlightRecovered, blendMask);
        
        // Only apply recovery where values are in highlight range (component-wise step)
        vec3 highlightRange = step(vec3(0.5), value);
        recovered = mix(value, recovered, highlightRange);
        
        return clamp01(recovered);
    } else {
        // Brighten highlights - push toward white while preserving color
        float maxChannel = max(value.r, max(value.g, value.b));
        float expansionFactor = smoothstep01(0.5, 1.0, maxChannel);
        vec3 brightened = value + (vec3(1.0) - value) * influence * expansionFactor;
        return clamp01(brightened);
    }
}

// Shadow recovery using proper tone curve - reveals detail in underexposed areas
vec3 applyShadowLift(vec3 value, float amount, float weight, float luminance) {
    if (abs(amount) < kEpsilon || weight < kEpsilon) return value;
    
    // Shadow mask - stronger for dark areas
    float shadowMask = smoothstep01(0.0, 0.55, 1.0 - luminance);
    float influence = amount * weight * shadowMask;
    
    if (amount > 0.0) {
        // Recover shadows: expand shadow range to reveal detail
        // Use a toe curve that lifts shadows while preserving mid-tones
        
        // Determine shadow amount for each channel (component-wise smoothstep)
        vec3 shadowAmount = smoothstep(vec3(0.0), vec3(0.5), vec3(1.0) - value);
        
        // Calculate expansion per channel
        vec3 expansion = vec3(influence) * shadowAmount;
        
        // Calculate toe gamma for expansion (gamma < 1 on inverted curve expands shadows)
        // Use inverse gamma: y = x^(1/gamma) expands the lower portion
        vec3 toeGamma = vec3(1.0) + expansion * 1.2;
        
        // Apply shadow recovery curve per channel
        // Only affect values below 0.5 (shadow range)
        vec3 recovered = value;
        
        // Normalize shadow range [0, 0.5] to [0, 1]
        vec3 normalized = clamp01(value / vec3(0.5));
        
        // Apply expansion using toe curve: pow(x, 1/gamma) expands lower values
        vec3 expanded = pow(normalized, vec3(1.0) / toeGamma);
        
        // Calculate lift amount - remap [0, 0.5] to [lift, 0.5]
        // More expansion means more lift from 0
        vec3 liftAmount = vec3(0.5) * expansion * 0.8;
        vec3 shadowRecovered = expanded * (vec3(0.5) - liftAmount) + liftAmount;
        
        // Mix original with recovered based on shadow mask and expansion amount
        vec3 blendMask = shadowAmount * vec3(influence * shadowMask);
        recovered = mix(value, shadowRecovered, blendMask);
        
        // Only apply recovery where values are in shadow range (component-wise step)
        // Values <= 0.5 are in shadow range
        vec3 shadowRange = vec3(1.0) - step(vec3(0.5), value);
        recovered = mix(value, recovered, shadowRange);
        
        return clamp01(recovered);
    } else {
        // Darken shadows - compress shadow range
        float darkenPower = 1.0 + abs(influence) * 1.5;
        return clamp01(pow(value, vec3(darkenPower)));
    }
}

// ============================================================================
// HSL Color Space Functions
// ============================================================================
)"
R"(

// Fast HSL to RGB conversion
float hueToRgb(float p, float q, float t) {
    t = fract(t);  // Wrap to [0,1] range efficiently
    if (t < 0.16666667) return p + (q - p) * 6.0 * t;
    if (t < 0.5) return q;
    if (t < 0.66666667) return p + (q - p) * (0.66666667 - t) * 6.0;
    return p;
}

// RGB to HSL and apply adjustments in one pass
vec3 applyHSLAdjustments(vec3 rgb, float hueShift, float satShift, float lumShift) {
    // Early exit if no adjustments
    if (abs(hueShift) < kEpsilon && abs(satShift) < kEpsilon && abs(lumShift) < kEpsilon) {
        return rgb;
    }
    
    // RGB to HSL conversion (optimized)
    float maxChannel = max(rgb.r, max(rgb.g, rgb.b));
    float minChannel = min(rgb.r, min(rgb.g, rgb.b));
    float chroma = maxChannel - minChannel;
    float luminance = (maxChannel + minChannel) * 0.5;
    
    // Handle grayscale case early
    if (chroma < kEpsilon) {
        luminance = clamp01(luminance + lumShift);
        return vec3(luminance);
    }
    
    // Calculate hue
    float hue;
    if (maxChannel == rgb.r) {
        hue = (rgb.g - rgb.b) / chroma;
        hue += (rgb.g < rgb.b) ? 6.0 : 0.0;
    } else if (maxChannel == rgb.g) {
        hue = 2.0 + (rgb.b - rgb.r) / chroma;
    } else {
        hue = 4.0 + (rgb.r - rgb.g) / chroma;
    }
    hue /= 6.0;
    
    // Calculate and adjust saturation
    float saturation = chroma / (1.0 - abs(2.0 * luminance - 1.0) + kEpsilon);
    saturation = clamp01(saturation + satShift);
    
    // Adjust hue and luminance
    hue = fract(hue + hueShift);
    luminance = clamp01(luminance + lumShift);
    
    // HSL to RGB conversion (optimized)
    float q = (luminance < 0.5) 
        ? luminance * (1.0 + saturation) 
        : luminance + saturation - luminance * saturation;
    float p = 2.0 * luminance - q;
    
    return vec3(
        hueToRgb(p, q, hue + 0.33333333),
        hueToRgb(p, q, hue),
        hueToRgb(p, q, hue - 0.33333333)
    );
}

// ============================================================================
// Main Tone Adjustment Pipeline
// ============================================================================

vec3 applyToneAdjustments(vec3 value, float luminance) {
    vec3 v = value;
    
    // 1. Contrast adjustment (around middle gray)
    v = (v - vec3(0.5)) * contrastFactor + vec3(0.5);
    
    // 2. Calculate tone range weights (optimized smoothstep ranges)
    float highlightWeight = smoothstep01(0.40, 0.90, luminance);
    float shadowWeight = smoothstep01(0.10, 0.50, 1.0 - luminance);
    float whitesWeight = smoothstep01(0.70, 0.98, luminance);
    float blacksWeight = smoothstep01(0.02, 0.30, 1.0 - luminance);
    
    // 3. Apply basic tone adjustments
    v = applyHighlightAdjust(v, highlights, highlightWeight, luminance);
    v = applyShadowLift(v, shadows, shadowWeight, luminance);
    
    // Apply whites/blacks with proper tone curves (preserves detail)
    v = applyWhitesAdjust(v, whites, whitesWeight);
    v = applyBlacksAdjust(v, blacks, blacksWeight);
    
    // 4. Tone curve adjustments (secondary pass)
    float toneHighlight = smoothstep01(0.50, 0.90, luminance);
    float toneLight = smoothstep01(0.35, 0.75, luminance);
    float toneDark = smoothstep01(0.25, 0.65, 1.0 - luminance);
    float toneShadow = smoothstep01(0.0, 0.35, 1.0 - luminance);
    
    v = applyHighlightAdjust(v, toneCurveHighlights, toneHighlight, luminance);
    
    v.r = applyRange(v.r, toneCurveLights, toneLight);
    v.g = applyRange(v.g, toneCurveLights, toneLight);
    v.b = applyRange(v.b, toneCurveLights, toneLight);
    
    v.r = applyRange(v.r, toneCurveDarks, toneDark);
    v.g = applyRange(v.g, toneCurveDarks, toneDark);
    v.b = applyRange(v.b, toneCurveDarks, toneDark);
    
    v = applyShadowLift(v, toneCurveShadows, toneShadow, luminance);
    
    return clamp01(v);
}

// ============================================================================
// Main Compute Shader Entry Point - Optimized Processing Pipeline
// ============================================================================

void main() {
    // Early exit for out-of-bounds threads
    ivec2 coord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(inputImage);
    if (coord.x >= size.x || coord.y >= size.y) {
        return;
    }

    // Load source pixel
    vec4 src = imageLoad(inputImage, coord);
    vec3 rgb = src.rgb * exposureMultiplier;
    
    // Calculate initial luminance
    float luminance = clamp01(getLuminance(rgb));

    // 1. Apply tone adjustments (contrast, highlights, shadows, etc.)
    rgb = applyToneAdjustments(rgb, luminance);

    // 2. Apply clarity (local contrast enhancement in mid-tones)
    if (applyClarity == 1 && abs(clarityStrength) > kEpsilon) {
        float newLum = getLuminance(rgb);
        float midToneWeight = 1.0 - abs(newLum - 0.5) * 2.0;
        float clarityFactor = 1.0 + clarityStrength * midToneWeight;
        rgb = clamp01((rgb - vec3(newLum)) * clarityFactor + vec3(newLum));
    }

    // 3. Apply saturation and vibrance
    if (abs(saturationFactor - 1.0) > kEpsilon || abs(vibranceAmount) > kEpsilon) {
        float maxChannel = max(rgb.r, max(rgb.g, rgb.b));
        float minChannel = min(rgb.r, min(rgb.g, rgb.b));
        float chroma = maxChannel - minChannel;
        float currentSat = (maxChannel > kEpsilon) ? chroma / maxChannel : 0.0;
        
        // Calculate combined saturation factor
        float combinedSat = saturationFactor;
        if (abs(vibranceAmount) > kEpsilon) {
            // Vibrance: protect already saturated colors
            float vibranceMask = (vibranceAmount > 0.0) 
                ? (1.0 - currentSat)  // Boost desaturated more
                : currentSat;          // Reduce saturated more
            combinedSat *= 1.0 + vibranceAmount * vibranceMask;
        }
        
        // Apply saturation
        float lum = getLuminance(rgb);
        rgb = clamp01(vec3(lum) + (rgb - vec3(lum)) * combinedSat);
    }

    // 4. Apply HSL adjustments
    rgb = applyHSLAdjustments(rgb, hueShift, saturationShift, luminanceShift);

    // 5. Apply sharpening (using original image data for edge detection)
    if (applySharpening == 1 && sharpening > kEpsilon) {
        // Clamp coordinates for safe neighbor access
        ivec2 left = ivec2(max(0, coord.x - 1), coord.y);
        ivec2 right = ivec2(min(imageWidth - 1, coord.x + 1), coord.y);
        ivec2 up = ivec2(coord.x, max(0, coord.y - 1));
        ivec2 down = ivec2(coord.x, min(imageHeight - 1, coord.y + 1));
        
        // Load neighbors from original image
        vec3 center = imageLoad(inputImage, coord).rgb * exposureMultiplier;
        vec3 neighbors = imageLoad(inputImage, left).rgb * exposureMultiplier
                       + imageLoad(inputImage, right).rgb * exposureMultiplier
                       + imageLoad(inputImage, up).rgb * exposureMultiplier
                       + imageLoad(inputImage, down).rgb * exposureMultiplier;
        
        // Unsharp mask: enhance edges
        vec3 edge = center * 4.0 - neighbors;
        rgb = clamp01(rgb + edge * sharpening * 0.5);
    }

    // 6. Apply noise reduction (luminance-based blur)
    if (applyNoiseReduction == 1 && noiseReduction > kEpsilon) {
        float lum = getLuminance(rgb);
        float blendFactor = clamp01(noiseReduction * 0.4);
        rgb = mix(rgb, vec3(lum), blendFactor);
    }

    // 7. Apply film grain (pseudo-random noise)
    if (applyGrain == 1 && grainAmount > kEpsilon) {
        // High-quality pseudo-random noise generator
        uint seed = uint(coord.x) * 1973u + uint(coord.y) * 9277u + 0x7f4a7c15u;
        seed = (seed << 13u) ^ seed;
        seed = seed * (seed * seed * 15731u + 789221u) + 1376312589u;
        float noise = (float(seed & 0x7fffffffu) / float(0x7fffffffu) - 0.5) * grainAmount;
        rgb = clamp01(rgb + vec3(noise));
    }

    // 8. Apply vignette (radial darkening/lightening)
    if (abs(vignetteStrength) > kEpsilon) {
        float dx = (float(coord.x) - centerX) * widthInv;
        float dy = (float(coord.y) - centerY) * heightInv;
        float dist = sqrt(dx * dx + dy * dy);
        float falloff = clamp01(dist * vignetteFalloff);
        float influence = vignetteStrength * falloff * falloff;
        
        rgb = (influence > 0.0)
            ? rgb + (vec3(1.0) - rgb) * influence  // Lighten
            : rgb * (1.0 + influence);              // Darken
    }

    // Write final result
    imageStore(outputImage, coord, vec4(clamp01(rgb), src.a));
}
)";

// ============================================================================
// CPU-side Helper Functions
// ============================================================================

// Fast clamp to [0, 1] with branch prediction hints
inline float clamp01(float value)
{
    return (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
}

// Optimized smoothstep implementation
inline float smoothstep(float edge0, float edge1, float x)
{
    if (edge0 == edge1) {
        return x >= edge1 ? 1.0f : 0.0f;
    }
    const float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

// Pre-computed adjustment parameters for GPU upload
// Aligned to 16-byte boundaries for optimal GPU memory access
struct alignas(16) AdjustmentPrecompute
{
    // Exposure and tone
    float exposureMultiplier = 1.0f;
    float contrastFactor = 1.0f;
    float highlights = 0.0f;
    float shadows = 0.0f;
    
    // Whites and blacks
    float whites = 0.0f;
    float blacks = 0.0f;
    float clarityStrength = 0.0f;
    float _pad1 = 0.0f;  // Alignment padding
    
    // Saturation
    float saturationFactor = 1.0f;
    float vibranceAmount = 0.0f;
    float _pad2[2] = {0.0f, 0.0f};  // Alignment padding
    
    // Tone curve
    float toneCurveHighlights = 0.0f;
    float toneCurveLights = 0.0f;
    float toneCurveDarks = 0.0f;
    float toneCurveShadows = 0.0f;
    
    // HSL adjustments
    float hueShift = 0.0f;
    float saturationShift = 0.0f;
    float luminanceShift = 0.0f;
    float _pad3 = 0.0f;  // Alignment padding
    
    // Effects
    float sharpening = 0.0f;
    float noiseReduction = 0.0f;
    float vignetteStrength = 0.0f;
    float vignetteFalloff = 1.0f;
    
    // Grain and geometry
    float grainAmount = 0.0f;
    float invWidth = 0.0f;
    float invHeight = 0.0f;
    float _pad4 = 0.0f;  // Alignment padding
    
    // Center point
    float centerX = 0.0f;
    float centerY = 0.0f;
    float _pad5[2] = {0.0f, 0.0f};  // Alignment padding
};

// Build pre-computed adjustment parameters - optimized conversions
AdjustmentPrecompute buildPrecompute(const DevelopAdjustments &adjustments, int width, int height)
{
    AdjustmentPrecompute pre;
    
    // Exposure: convert stops to linear multiplier (2^exposure)
    pre.exposureMultiplier = std::exp2(static_cast<float>(adjustments.exposure));

    // Contrast: improved curve for better results
    const float contrast = static_cast<float>(adjustments.contrast) * 0.01f;
    if (contrast >= 0.0f) {
        // Positive contrast: steeper S-curve
        pre.contrastFactor = 1.0f + contrast * 2.0f;
    } else {
        // Negative contrast: flatten S-curve
        pre.contrastFactor = 1.0f / (1.0f - contrast * 0.95f);
    }
    pre.contrastFactor = std::max(0.05f, std::min(pre.contrastFactor, 10.0f));

    // Tone adjustments: normalize to [-1, 1] range
    pre.highlights = static_cast<float>(adjustments.highlights) * 0.01f;
    pre.shadows = static_cast<float>(adjustments.shadows) * 0.01f;
    pre.whites = static_cast<float>(adjustments.whites) * 0.01f;
    pre.blacks = static_cast<float>(adjustments.blacks) * 0.01f;

    // Clarity: reduced default strength for more natural look
    pre.clarityStrength = static_cast<float>(adjustments.clarity) * 0.005f;
    
    // Saturation and vibrance
    pre.saturationFactor = 1.0f + static_cast<float>(adjustments.saturation) * 0.01f;
    pre.saturationFactor = std::max(0.0f, std::min(pre.saturationFactor, 3.0f));
    pre.vibranceAmount = static_cast<float>(adjustments.vibrance) * 0.01f;

    // Tone curve adjustments
    pre.toneCurveHighlights = static_cast<float>(adjustments.toneCurveHighlights) * 0.01f;
    pre.toneCurveLights = static_cast<float>(adjustments.toneCurveLights) * 0.01f;
    pre.toneCurveDarks = static_cast<float>(adjustments.toneCurveDarks) * 0.01f;
    pre.toneCurveShadows = static_cast<float>(adjustments.toneCurveShadows) * 0.01f;

    // HSL adjustments
    pre.hueShift = static_cast<float>(adjustments.hueShift) / 360.0f;
    pre.saturationShift = static_cast<float>(adjustments.saturationShift) * 0.01f;
    pre.luminanceShift = static_cast<float>(adjustments.luminanceShift) * 0.01f;

    // Detail adjustments
    pre.sharpening = static_cast<float>(adjustments.sharpening) * 0.01f;
    pre.noiseReduction = static_cast<float>(adjustments.noiseReduction) * 0.01f;

    // Vignette
    pre.vignetteStrength = static_cast<float>(adjustments.vignette) * 0.01f;
    pre.vignetteFalloff = 1.5f;  // Standard falloff
    
    // Film grain
    pre.grainAmount = static_cast<float>(adjustments.grain) * 0.0003f;

    // Image geometry (pre-computed for efficiency)
    const float fWidth = static_cast<float>(width);
    const float fHeight = static_cast<float>(height);
    pre.invWidth = (width > 0) ? 1.0f / fWidth : 0.0f;
    pre.invHeight = (height > 0) ? 1.0f / fHeight : 0.0f;
    pre.centerX = fWidth * 0.5f;
    pre.centerY = fHeight * 0.5f;

    return pre;
}

} // namespace

// Static shared GPU state - allows worker threads to use GPU initialized by main thread
namespace {
    QOpenGLContext* s_sharedGlContext = nullptr;
    QOffscreenSurface* s_sharedOffscreenSurface = nullptr;
    GLuint s_sharedComputeProgram = 0;
    bool s_gpuInitialized = false;
    QMutex s_sharedGpuMutex;
}

DevelopAdjustmentEngine::DevelopAdjustmentEngine(QObject *parent)
    : QObject(parent)
{
    qDebug() << "DevelopAdjustmentEngine::DevelopAdjustmentEngine: Constructor called from thread:"
             << QThread::currentThread() << "isMainThread:"
             << (QThread::currentThread() == QCoreApplication::instance()->thread());
    
    // Check if GPU was already initialized by another instance
    QMutexLocker locker(&s_sharedGpuMutex);
    if (s_gpuInitialized && s_sharedGlContext && s_sharedOffscreenSurface) {
        // Use the shared GPU resources - we'll reference them via getOrCreateThreadContext()
        // Don't take ownership, just mark that GPU is available and we'll use shared context
        m_computeProgram = s_sharedComputeProgram;
        m_gpuInitialized = true;
        m_gpuAvailable = true;
        // Store references (won't be deleted - shared context handles that)
        // We'll access the actual context via getOrCreateThreadContext() in renderWithGpu()
        qDebug() << "DevelopAdjustmentEngine::DevelopAdjustmentEngine: Using shared GPU context from another instance";
    }
}

void DevelopAdjustmentEngine::initializeGpuOnMainThread()
{
    qDebug() << "DevelopAdjustmentEngine::initializeGpuOnMainThread: Called from thread:"
             << QThread::currentThread() << "isMainThread:"
             << (QThread::currentThread() == QCoreApplication::instance()->thread());
    
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        qWarning() << "DevelopAdjustmentEngine::initializeGpuOnMainThread: NOT on main thread! This will fail.";
    }
    
    // Initialize GPU on the main thread
    initializeGpu();
}

DevelopAdjustmentEngine::~DevelopAdjustmentEngine()
{
    cancelActive();
    if (m_computeProgram != 0 && m_glContext) {
        QMutexLocker locker(&m_glMutex);
        if (m_glContext->makeCurrent(m_offscreenSurface.get())) {
            QOpenGLFunctions_4_3_Core funcs;
            if (funcs.initializeOpenGLFunctions()) {
                funcs.glDeleteProgram(m_computeProgram);
            }
            m_glContext->doneCurrent();
        }
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
    qDebug() << "DevelopAdjustmentEngine::startRender: Starting render for requestId:" << request.requestId
             << "from thread:" << QThread::currentThread()
             << "isMainThread:" << (QThread::currentThread() == QCoreApplication::instance()->thread());
    
    return QtConcurrent::run([this, request = std::move(request), token]() mutable {
        qDebug() << "DevelopAdjustmentEngine::startRender: Lambda executing on thread:" << QThread::currentThread()
                 << "isMainThread:" << (QThread::currentThread() == QCoreApplication::instance()->thread())
                 << "requestId:" << request.requestId;
        
        DevelopAdjustmentRenderResult result;
        if (initializeGpu()) {
            result = renderWithGpu(request, token);
        } else {
            qWarning() << "DevelopAdjustmentEngine::startRender: GPU initialization failed for requestId:" << request.requestId;
            result.cancelled = true;
            result.errorMessage = QStringLiteral("GPU initialization failed");
        }
        result.requestId = request.requestId;
        result.isPreview = request.isPreview;
        result.displayScale = request.displayScale;
        
        qDebug() << "DevelopAdjustmentEngine::startRender: Render complete for requestId:" << request.requestId
                 << "cancelled:" << result.cancelled << "hasError:" << !result.errorMessage.isEmpty();
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
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Called from thread:" << QThread::currentThread()
             << "isMainThread:" << (QThread::currentThread() == QCoreApplication::instance()->thread());
    
    if (m_gpuInitialized) {
        qDebug() << "DevelopAdjustmentEngine::initializeGpu: Already initialized, GPU available:" << m_gpuAvailable;
        return m_gpuAvailable;
    }
    
    // QOffscreenSurface must be created on the main thread
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        // Check if GPU was already initialized by another instance (on main thread)
        QMutexLocker locker(&s_sharedGpuMutex);
        if (s_gpuInitialized && s_sharedGlContext && s_sharedOffscreenSurface) {
            // Use the shared GPU resources from main thread - reference without taking ownership
            // We'll access the actual context via getOrCreateThreadContext() in renderWithGpu()
            m_computeProgram = s_sharedComputeProgram;
            m_gpuInitialized = true;
            m_gpuAvailable = true;
            qDebug() << "DevelopAdjustmentEngine::initializeGpu: Using shared GPU context on worker thread";
            return true;
        }
        
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: Called from non-main thread! QOffscreenSurface creation will fail.";
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: GPU must be initialized on main thread first via initializeGpuOnMainThread().";
        // Don't mark as initialized - allow future attempts if GPU gets initialized
        return false;
    }

    m_gpuInitialized = true;
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Starting GPU initialization";

    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setVersion(4, 3); // Minimum for compute shaders
    format.setOption(QSurfaceFormat::DebugContext, false);

    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Creating OpenGL context with format -"
             << "Version:" << format.majorVersion() << format.minorVersion()
             << "Profile:" << (format.profile() == QSurfaceFormat::CoreProfile ? "Core" : "Compatibility")
             << "Renderable:" << (format.renderableType() == QSurfaceFormat::OpenGL ? "OpenGL" : "OpenGLES");

    auto context = std::make_unique<QOpenGLContext>();
    context->setFormat(format);
    if (!context->create()) {
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: Failed to create OpenGL context";
        m_gpuAvailable = false;
        return false;
    }
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: OpenGL context created successfully";

    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Creating QOffscreenSurface from thread:"
             << QThread::currentThread() << "isMainThread:" 
             << (QThread::currentThread() == QCoreApplication::instance()->thread());
    
    auto surface = std::make_unique<QOffscreenSurface>();
    surface->setFormat(format);
    surface->create();

    if (!surface->isValid()) {
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: QOffscreenSurface is not valid";
        m_gpuAvailable = false;
        return false;
    }
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: QOffscreenSurface created and valid";

    if (!context->makeCurrent(surface.get())) {
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: Failed to make OpenGL context current";
        m_gpuAvailable = false;
        return false;
    }
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: OpenGL context made current";
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Context isValid:" << context->isValid();
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Context format:" << context->format().majorVersion() << context->format().minorVersion();
    
    // First, try to get basic OpenGL info using the context's functions
    QOpenGLFunctions* basicFuncs = context->functions();
    if (basicFuncs) {
        const GLubyte* versionStr = basicFuncs->glGetString(GL_VERSION);
        if (versionStr) {
            qDebug() << "DevelopAdjustmentEngine::initializeGpu: OpenGL version string:" << reinterpret_cast<const char*>(versionStr);
        } else {
            qWarning() << "DevelopAdjustmentEngine::initializeGpu: Could not get OpenGL version string";
        }
        
        const GLubyte* rendererStr = basicFuncs->glGetString(GL_RENDERER);
        if (rendererStr) {
            qDebug() << "DevelopAdjustmentEngine::initializeGpu: OpenGL renderer:" << reinterpret_cast<const char*>(rendererStr);
        }
        
        const GLubyte* vendorStr = basicFuncs->glGetString(GL_VENDOR);
        if (vendorStr) {
            qDebug() << "DevelopAdjustmentEngine::initializeGpu: OpenGL vendor:" << reinterpret_cast<const char*>(vendorStr);
        }
    } else {
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: Context functions() returned null";
    }

    // Use 4.3 functions since that's what the context supports (compute shaders available in 4.3+)
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Initializing OpenGL 4.3 Core functions";
    QOpenGLFunctions_4_3_Core funcs;
    if (!funcs.initializeOpenGLFunctions()) {
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: Failed to initialize OpenGL 4.3 Core functions";
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: This might indicate the OpenGL context doesn't support OpenGL 4.3";
        
        // Try to get the actual OpenGL version
        if (basicFuncs) {
            GLint majorVersion = 0, minorVersion = 0;
            basicFuncs->glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
            basicFuncs->glGetIntegerv(GL_MINOR_VERSION, &minorVersion);
            qWarning() << "DevelopAdjustmentEngine::initializeGpu: Actual OpenGL version:" << majorVersion << minorVersion;
        }
        
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: OpenGL 4.3 Core functions initialized successfully";

    // Check for compute shader support
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Checking compute shader support";
    GLint maxComputeWorkGroupInvocations = 0;
    funcs.glGetIntegerv(GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS, &maxComputeWorkGroupInvocations);
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Max compute work group invocations:" << maxComputeWorkGroupInvocations;
    if (maxComputeWorkGroupInvocations == 0) {
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: Compute shaders not supported";
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }

    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Creating compute shader";
    GLuint shader = funcs.glCreateShader(GL_COMPUTE_SHADER);
    if (shader == 0) {
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: Failed to create compute shader";
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
            qWarning() << "DevelopAdjustmentEngine::initializeGpu: Compute shader compilation failed:" << log.data();
        } else {
            qWarning() << "DevelopAdjustmentEngine::initializeGpu: Compute shader compilation failed (no log available)";
        }
        funcs.glDeleteShader(shader);
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Compute shader compiled successfully";

    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Creating compute program";
    GLuint program = funcs.glCreateProgram();
    if (program == 0) {
        qWarning() << "DevelopAdjustmentEngine::initializeGpu: Failed to create program";
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
            qWarning() << "DevelopAdjustmentEngine::initializeGpu: Compute shader program linking failed:" << log.data();
        } else {
            qWarning() << "DevelopAdjustmentEngine::initializeGpu: Compute shader program linking failed (no log available)";
        }
        funcs.glDeleteProgram(program);
        context->doneCurrent();
        m_gpuAvailable = false;
        return false;
    }
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: Compute program linked successfully";

    m_computeProgram = program;
    m_glContext = std::move(context);
    m_offscreenSurface = std::move(surface);
    m_glContext->doneCurrent();
    m_gpuAvailable = true;
    
    // Share GPU resources with other engine instances via static variables
    {
        QMutexLocker locker(&s_sharedGpuMutex);
        s_sharedGlContext = m_glContext.get();
        s_sharedOffscreenSurface = m_offscreenSurface.get();
        s_sharedComputeProgram = m_computeProgram;
        s_gpuInitialized = true;
    }
    
    qDebug() << "DevelopAdjustmentEngine::initializeGpu: GPU initialization complete, GPU available:" << m_gpuAvailable;
    return true;
}

QOpenGLContext* DevelopAdjustmentEngine::getOrCreateThreadContext() const
{
    if (!m_gpuAvailable) {
        return nullptr;
    }
    
    // Check if we already have a context for this thread
    if (m_threadContexts.hasLocalData()) {
        return m_threadContexts.localData();
    }
    
    // Get the shared context - either from this instance or from static shared state
    QOpenGLContext* shareContext = nullptr;
    QSurfaceFormat format;
    
    if (m_glContext) {
        // This instance owns the context
        shareContext = m_glContext.get();
        format = m_glContext->format();
    } else {
        // This instance is using shared GPU resources
        QMutexLocker locker(&s_sharedGpuMutex);
        if (s_gpuInitialized && s_sharedGlContext) {
            shareContext = s_sharedGlContext;
            format = s_sharedGlContext->format();
        } else {
            return nullptr;
        }
    }
    
    // Create a new shared context for this thread
    QOpenGLContext* threadContext = new QOpenGLContext();
    threadContext->setFormat(format);
    threadContext->setShareContext(shareContext);
    
    if (!threadContext->create()) {
        qWarning() << "DevelopAdjustmentEngine::getOrCreateThreadContext: Failed to create shared context for thread";
        delete threadContext;
        return nullptr;
    }
    
    // Create a surface for this thread
    QOffscreenSurface* threadSurface = new QOffscreenSurface();
    threadSurface->setFormat(format);
    threadSurface->create();
    
    if (!threadSurface->isValid()) {
        qWarning() << "DevelopAdjustmentEngine::getOrCreateThreadContext: Failed to create surface for thread";
        delete threadSurface;
        delete threadContext;
        return nullptr;
    }
    
    // Store in thread-local storage
    m_threadContexts.setLocalData(threadContext);
    m_threadSurfaces.setLocalData(threadSurface);
    
    qDebug() << "DevelopAdjustmentEngine::getOrCreateThreadContext: Created shared context for thread:" << QThread::currentThread();
    return threadContext;
}

DevelopAdjustmentRenderResult DevelopAdjustmentEngine::renderWithGpu(const DevelopAdjustmentRequest &request,
                                                                     const std::shared_ptr<CancellationToken> &token)
{
    QElapsedTimer timer;
    timer.start();

    DevelopAdjustmentRenderResult result;
    result.requestId = request.requestId;
    result.isPreview = request.isPreview;
    result.displayScale = request.displayScale;

    // Early cancellation check
    if (token && token->cancelled.load(std::memory_order_acquire)) {
        result.cancelled = true;
        return result;
    }

    // Validate GPU context - either owned by this instance or shared from another
    if (m_computeProgram == 0) {
        // Check if using shared GPU resources
        QMutexLocker locker(&s_sharedGpuMutex);
        if (!s_gpuInitialized || !s_sharedGlContext || s_sharedComputeProgram == 0) {
            result.cancelled = true;
            result.errorMessage = QStringLiteral("GPU context not available");
            return result;
        }
        // Update from shared state if this instance doesn't have it
        m_computeProgram = s_sharedComputeProgram;
    }

    // Get or create thread-local shared context for multi-threaded rendering
    QOpenGLContext* threadContext = getOrCreateThreadContext();
    if (!threadContext) {
        result.cancelled = true;
        result.errorMessage = QStringLiteral("Failed to create thread context");
        return result;
    }
    
    QOffscreenSurface* threadSurface = m_threadSurfaces.localData();
    if (!threadSurface || !threadSurface->isValid()) {
        result.cancelled = true;
        result.errorMessage = QStringLiteral("Thread surface not available");
        return result;
    }

    // Acquire GPU context (thread-safe)
    QMutexLocker locker(&m_glMutex);
    if (!threadContext->makeCurrent(threadSurface)) {
        qWarning() << "DevelopAdjustmentEngine::renderWithGpu: Failed to make context current";
        result.cancelled = true;
        result.errorMessage = QStringLiteral("Failed to make GPU context current");
        return result;
    }

    // Initialize OpenGL functions
    QOpenGLFunctions_4_3_Core funcs;
    if (!funcs.initializeOpenGLFunctions()) {
        threadContext->doneCurrent();
        result.cancelled = true;
        result.errorMessage = QStringLiteral("Failed to initialize OpenGL functions");
        return result;
    }

    // Convert image to RGBA format if needed (avoid unnecessary conversions)
    QImage sourceImage;
    if (request.image.format() == QImage::Format_RGBA8888 || 
        request.image.format() == QImage::Format_RGBA8888_Premultiplied) {
        sourceImage = request.image;
    } else {
        sourceImage = request.image.convertToFormat(QImage::Format_RGBA8888);
    }
    
    const int width = sourceImage.width();
    const int height = sourceImage.height();
    const int pixelCount = width * height;

    // Validate dimensions
    if (width <= 0 || height <= 0 || width > kMaxTextureDimension || height > kMaxTextureDimension) {
        threadContext->doneCurrent();
        result.cancelled = true;
        result.errorMessage = QStringLiteral("Image dimensions out of range");
        return result;
    }

    // Optimized texture data conversion (vectorized when possible)
    std::vector<float> textureData;
    textureData.resize(pixelCount * 4);
    const uchar *srcBits = sourceImage.constBits();
    
    // Convert 8-bit to float in chunks for better cache utilization
    constexpr int chunkSize = 64;  // Process 64 pixels at a time
    const int fullChunks = pixelCount / chunkSize;
    const int remainder = pixelCount % chunkSize;
    
    for (int chunk = 0; chunk < fullChunks; ++chunk) {
        const int baseIdx = chunk * chunkSize * 4;
        for (int i = 0; i < chunkSize * 4; ++i) {
            textureData[baseIdx + i] = srcBits[baseIdx + i] * kInv255;
        }
    }
    
    // Handle remainder pixels
    const int remainderBase = fullChunks * chunkSize * 4;
    for (int i = 0; i < remainder * 4; ++i) {
        textureData[remainderBase + i] = srcBits[remainderBase + i] * kInv255;
    }

    // Check for cancellation before expensive GPU operations
    if (token && token->cancelled.load(std::memory_order_acquire)) {
        threadContext->doneCurrent();
        result.cancelled = true;
        return result;
    }

    // Create GPU textures
    GLuint textures[2] = {0, 0};
    funcs.glGenTextures(2, textures);
    GLuint inputTex = textures[0];
    GLuint outputTex = textures[1];

    // RAII-style texture cleanup
    auto releaseTextures = [&]() {
        funcs.glDeleteTextures(2, textures);
    };

    // Configure input texture (immutable storage for better performance)
    funcs.glBindTexture(GL_TEXTURE_2D, inputTex);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    funcs.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, width, height);
    funcs.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, textureData.data());

    // Configure output texture (immutable storage)
    funcs.glBindTexture(GL_TEXTURE_2D, outputTex);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    funcs.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    funcs.glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, width, height);

    // Activate compute shader program
    funcs.glUseProgram(m_computeProgram);

    // Build pre-computed adjustments
    const AdjustmentPrecompute pre = buildPrecompute(request.adjustments, width, height);

    // Optimized uniform setting with cached locations (could be further optimized with UBOs)
    auto setUniform = [&](const char *name, float value) {
        const GLint loc = funcs.glGetUniformLocation(m_computeProgram, name);
        if (loc >= 0) {
            funcs.glUniform1f(loc, value);
        }
    };
    
    auto setUniformInt = [&](const char *name, int value) {
        const GLint loc = funcs.glGetUniformLocation(m_computeProgram, name);
        if (loc >= 0) {
            funcs.glUniform1i(loc, value);
        }
    };

    // Set all uniforms (grouped by category for better cache locality)
    // Exposure and tone
    setUniform("exposureMultiplier", pre.exposureMultiplier);
    setUniform("contrastFactor", pre.contrastFactor);
    setUniform("highlights", pre.highlights);
    setUniform("shadows", pre.shadows);
    setUniform("whites", pre.whites);
    setUniform("blacks", pre.blacks);
    
    // Saturation
    setUniform("saturationFactor", pre.saturationFactor);
    setUniform("vibranceAmount", pre.vibranceAmount);
    
    // Tone curve
    setUniform("toneCurveHighlights", pre.toneCurveHighlights);
    setUniform("toneCurveLights", pre.toneCurveLights);
    setUniform("toneCurveDarks", pre.toneCurveDarks);
    setUniform("toneCurveShadows", pre.toneCurveShadows);
    
    // HSL
    setUniform("hueShift", pre.hueShift);
    setUniform("saturationShift", pre.saturationShift);
    setUniform("luminanceShift", pre.luminanceShift);
    
    // Effects
    setUniform("clarityStrength", pre.clarityStrength);
    setUniform("sharpening", pre.sharpening);
    setUniform("noiseReduction", pre.noiseReduction);
    setUniform("vignetteStrength", pre.vignetteStrength);
    setUniform("vignetteFalloff", pre.vignetteFalloff);
    setUniform("grainAmount", pre.grainAmount);
    
    // Geometry
    setUniform("widthInv", pre.invWidth);
    setUniform("heightInv", pre.invHeight);
    setUniform("centerX", pre.centerX);
    setUniform("centerY", pre.centerY);
    
    // Feature flags (skip expensive operations in preview mode)
    setUniformInt("applyClarity", request.isPreview ? 0 : 1);
    setUniformInt("applySharpening", (request.isPreview || pre.sharpening < 0.01f) ? 0 : 1);
    setUniformInt("applyNoiseReduction", (request.isPreview || pre.noiseReduction < 0.01f) ? 0 : 1);
    setUniformInt("applyGrain", (pre.grainAmount > 0.0f) ? 1 : 0);
    setUniformInt("imageWidth", width);
    setUniformInt("imageHeight", height);

    // Bind textures as image units for compute shader access
    funcs.glBindImageTexture(0, inputTex, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA32F);
    funcs.glBindImageTexture(1, outputTex, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA32F);

    // Dispatch compute shader (calculate work groups)
    const GLuint groupsX = (width + kWorkgroupSize - 1) / kWorkgroupSize;
    const GLuint groupsY = (height + kWorkgroupSize - 1) / kWorkgroupSize;
    funcs.glDispatchCompute(groupsX, groupsY, 1);

    // Memory barrier to ensure compute shader writes are visible
    funcs.glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

    // Check for cancellation after compute dispatch
    if (token && token->cancelled.load(std::memory_order_acquire)) {
        releaseTextures();
        funcs.glUseProgram(0);
        threadContext->doneCurrent();
        result.cancelled = true;
        return result;
    }

    // Read back results from GPU
    std::vector<float> outputData;
    outputData.resize(pixelCount * 4);
    funcs.glBindTexture(GL_TEXTURE_2D, outputTex);
    funcs.glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, outputData.data());

    // Cleanup GPU resources
    releaseTextures();
    funcs.glUseProgram(0);
    threadContext->doneCurrent();

    // Convert float data back to 8-bit QImage (optimized)
    QImage outputImage(width, height, QImage::Format_RGBA8888);
    outputImage.detach();
    uchar *dstBits = outputImage.bits();
    
    // Vectorized conversion with better cache utilization
    for (int chunk = 0; chunk < fullChunks; ++chunk) {
        const int baseIdx = chunk * chunkSize * 4;
        for (int i = 0; i < chunkSize * 4; ++i) {
            const float val = std::clamp(outputData[baseIdx + i], 0.0f, 1.0f);
            dstBits[baseIdx + i] = static_cast<uchar>(val * k255 + 0.5f);
        }
    }
    
    // Handle remainder pixels
    for (int i = 0; i < remainder * 4; ++i) {
        const float val = std::clamp(outputData[remainderBase + i], 0.0f, 1.0f);
        dstBits[remainderBase + i] = static_cast<uchar>(val * k255 + 0.5f);
    }

    result.elapsedMs = timer.elapsed();
    result.image = std::move(outputImage);
    return result;
}


