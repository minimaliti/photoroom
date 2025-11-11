#include "developadjustmentengine.h"

#include <QtConcurrent/QtConcurrentMap>
#include <QtConcurrent/QtConcurrentRun>
#include <QElapsedTimer>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr float kInv255 = 1.0f / 255.0f;

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

DevelopAdjustmentRenderResult renderImage(const QImage &source,
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
    return QtConcurrent::run([request = std::move(request), token]() mutable {
        DevelopAdjustmentRenderResult result = renderImage(request.image, request.adjustments, request.isPreview, token);
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


