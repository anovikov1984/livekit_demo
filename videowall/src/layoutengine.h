#pragma once
#include <QRect>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

struct JustifiedConfig {
    int   gap{4};
    float minRowH{80.0f};
    float maxRowH{std::numeric_limits<float>::max()};
};

class LayoutEngine {
public:
    // Expose config type for callers (e.g. VideoGrid).
    using JustifiedConfig = ::JustifiedConfig;

    // Main entry point. Backward-compatible with all existing callers.
    // When aspects is non-empty and activeIdx < 0, uses justified layout.
    std::vector<QRect> compute(
            int n, int w, int h,
            int activeIdx = -1,
            const std::vector<float>& aspects = {},
            const JustifiedConfig& cfg = {}) const
    {
        if (n <= 0 || w <= 0 || h <= 0) return {};
        if (n == 1) return {QRect(0, 0, w, h)};
        const QRect container(0, 0, w, h);
        if (activeIdx >= 0 && activeIdx < n)
            return dominantLayout(n, container, activeIdx);
        if (!aspects.empty())
            return computeJustified(aspects, w, h, cfg);
        return equalGrid(n, container);
    }

    // Justified layout: each row fills width exactly; tile widths proportional
    // to aspect ratio. All tiles in a row share the same height.
    std::vector<QRect> computeJustified(
            const std::vector<float>& aspects,
            int w, int h,
            const JustifiedConfig& cfg = {}) const
    {
        const int n = static_cast<int>(aspects.size());
        if (n <= 0 || w <= 0 || h <= 0) return {};
        if (n == 1) return {QRect(0, 0, w, h)};

        std::vector<float> safe(n);
        for (int i = 0; i < n; ++i) safe[i] = sanitizeAspect(aspects[i]);

        const auto rows = justifiedRows(safe, w, h, cfg);
        const int rowCount = static_cast<int>(rows.size());
        if (rowCount == 0) return {};

        // Divide available height equally across rows so no row dominates.
        // (Proportional-to-natural-height weighting gave a lone-stream last row
        // 2× the height of multi-stream rows, which looked unbalanced.)
        const int totalGapH = cfg.gap * (rowCount - 1);
        const int availableH = std::max(rowCount, h - totalGapH);
        std::vector<int> rowH(rowCount);
        {
            const int baseH = availableH / rowCount;
            int allocated = 0;
            for (int ri = 0; ri < rowCount - 1; ++ri) {
                rowH[ri] = baseH;
                allocated += baseH;
            }
            rowH[rowCount - 1] = availableH - allocated;
        }

        // Place tiles using the scaled row heights.
        std::vector<QRect> result(n);
        int y = 0;
        for (int ri = 0; ri < rowCount; ++ri) {
            const auto& row = rows[ri];
            const int m = static_cast<int>(row.indices.size());
            const int rh = rowH[ri];

            // Tile widths: proportional to aspect ratios, scaled to fill usableW.
            // Using usableW*a/sumA (not rh*a) keeps widths correct after vertical scaling.
            const float usableWf = static_cast<float>(w - cfg.gap * (m - 1));
            int x = 0;
            for (int j = 0; j < m; ++j) {
                const int si = row.indices[j];
                const int tileW = (j == m - 1)
                    ? w - x
                    : static_cast<int>(std::round(usableWf * safe[si] / row.aspectSum));
                result[si] = QRect(x, y, tileW, rh);
                x += tileW + cfg.gap;
            }
            y += rh;
            if (ri < rowCount - 1) y += cfg.gap;
        }
        return result;
    }

private:
    struct GridDims { int rows, cols; };
    struct Row { std::vector<int> indices; float aspectSum{0.f}; };

    static float sanitizeAspect(float a) {
        if (!std::isfinite(a) || a <= 0.f) return 16.f / 9.f;
        return std::clamp(a, 0.1f, 10.f);
    }

    // Greedy row-formation. Uses a container-aware target height so rows spread
    // evenly across both width and height.
    // targetH = sqrt(W*H / (n*avgAspect)) — the height that makes tiles "square-ish"
    // given the container dimensions and stream count.
    std::vector<Row> justifiedRows(
            const std::vector<float>& safe,
            int w, int h,
            const JustifiedConfig& cfg) const
    {
        const int n = static_cast<int>(safe.size());
        const float W = static_cast<float>(w);

        float sumA = 0.f;
        for (float a : safe) sumA += a;
        const float avgA = sumA / static_cast<float>(n);
        const float targetH = std::clamp(
            std::sqrt(W * static_cast<float>(h) / (static_cast<float>(n) * avgA)),
            cfg.minRowH, cfg.maxRowH);

        std::vector<Row> rows;
        Row current;
        for (int i = 0; i < n; ++i) {
            const float newSum = current.aspectSum + safe[i];
            const int gapsIfAdded = static_cast<int>(current.indices.size());
            const float usableW = W - static_cast<float>(cfg.gap * gapsIfAdded);
            const float projectedH = (usableW > 0.f) ? usableW / newSum : 0.f;
            if (current.indices.empty() || projectedH >= targetH) {
                current.indices.push_back(i);
                current.aspectSum = newSum;
            } else {
                rows.push_back(std::move(current));
                current = Row{};
                current.indices.push_back(i);
                current.aspectSum = safe[i];
            }
        }
        if (!current.indices.empty()) rows.push_back(std::move(current));
        return rows;
    }

    // --- legacy methods preserved verbatim ---

    static float scoreGrid(int rows, int cols, int n, int w, int h) {
        const int empty = rows * cols - n;
        const float emptyPenalty = static_cast<float>(empty) / n;
        const float containerAspect = static_cast<float>(w) / h;
        const float gridAspect = static_cast<float>(cols) / rows;
        const float aspectPenalty = std::abs(containerAspect - gridAspect);
        return 1.0f * emptyPenalty + 0.5f * aspectPenalty;
    }

    GridDims bestGrid(int n, int w, int h) const {
        const int maxCols = static_cast<int>(std::ceil(std::sqrt(static_cast<double>(n)))) + 1;
        GridDims best{1, n};
        float bestScore = scoreGrid(1, n, n, w, h);
        for (int cols = 1; cols <= maxCols; ++cols) {
            const int rows = (n + cols - 1) / cols;
            if (rows < 1) continue;
            const float s = scoreGrid(rows, cols, n, w, h);
            if (s < bestScore) { bestScore = s; best = {rows, cols}; }
        }
        return best;
    }

    std::vector<QRect> equalGrid(int n, QRect c) const {
        const auto [rows, cols] = bestGrid(n, c.width(), c.height());
        std::vector<QRect> rects;
        rects.reserve(n);
        const int cellH = c.height() / rows;
        for (int i = 0; i < n; ++i) {
            const int row = i / cols, col = i % cols;
            const int firstInRow = row * cols;
            const int cellsInRow = std::min(cols, n - firstInRow);
            const int cellW = c.width() / cellsInRow;
            const int x = c.x() + col * cellW, y = c.y() + row * cellH;
            const int w2 = (col == cellsInRow - 1) ? c.width() - col * cellW : cellW;
            const int h2 = (row == rows - 1) ? c.height() - row * cellH : cellH;
            rects.push_back(QRect(x, y, w2, h2));
        }
        return rects;
    }

    std::vector<QRect> dominantLayout(int n, QRect c, int activeIdx) const {
        const bool landscape = c.width() >= c.height();
        QRect primaryRect, secondaryRect;
        if (landscape) {
            const int primaryW = static_cast<int>(c.width() * 0.65f);
            primaryRect   = QRect(c.x(), c.y(), primaryW, c.height());
            secondaryRect = QRect(c.x() + primaryW, c.y(), c.width() - primaryW, c.height());
        } else {
            const int primaryH = static_cast<int>(c.height() * 0.65f);
            primaryRect   = QRect(c.x(), c.y(), c.width(), primaryH);
            secondaryRect = QRect(c.x(), c.y() + primaryH, c.width(), c.height() - primaryH);
        }
        const int secondaryCount = n - 1;
        std::vector<QRect> secondaryRects;
        if (secondaryCount > 0) secondaryRects = equalGrid(secondaryCount, secondaryRect);
        std::vector<QRect> result(n);
        result[activeIdx] = primaryRect;
        int secIdx = 0;
        for (int i = 0; i < n; ++i) {
            if (i == activeIdx) continue;
            result[i] = secondaryRects[secIdx++];
        }
        return result;
    }
};
