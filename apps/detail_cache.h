/// @file detail_cache.h
/// @brief 细节区域的 LRU 缓存策略（不依赖 GPU / ImGui，可单测）。
/// @details 只负责“选层、覆盖判断、LRU 时间戳、按帧收集”；纹理数组由渲染器持有。

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "image_model.h"

namespace voilut::app
{
    /// @brief 一个已缓存的细节区域（纹理数组层）。
    struct DetailCacheEntry
    {
        voilut::Rect rect;       ///< 原图坐标矩形。
        int32_t layer = -1;      ///< 纹理数组层号；<0 表示无效。
        uint32_t frame = 0;      ///< 帧序号。
        uint64_t last_used = 0;  ///< LRU 时间戳。
        bool valid = false;      ///< 是否有效。
    };

    /// @brief 固定容量的细节区域 LRU 缓存。
    class DetailCache
    {
    public:
        static constexpr std::size_t kCapacity = 4;

        /// @brief 选一个层：优先空闲层，否则最久未使用层。
        [[nodiscard]] int32_t pick_layer() const noexcept
        {
            int32_t free_layer = -1;
            int32_t lru_layer = 0;
            uint64_t oldest = UINT64_MAX;
            for (std::size_t i = 0; i < entries_.size(); ++i)
            {
                const DetailCacheEntry &entry = entries_[i];
                if (!entry.valid)
                {
                    free_layer = static_cast<int32_t>(i);
                    break;
                }
                if (entry.last_used < oldest)
                {
                    oldest = entry.last_used;
                    lru_layer = static_cast<int32_t>(i);
                }
            }
            return free_layer >= 0 ? free_layer : lru_layer;
        }

        /// @brief 是否有同帧的有效层完整覆盖给定可见区域。
        [[nodiscard]] bool covers(double x0, double y0, double x1, double y1,
                                  uint32_t frame) const noexcept
        {
            for (const DetailCacheEntry &entry : entries_)
            {
                if (!entry.valid || entry.layer < 0 || entry.frame != frame)
                {
                    continue;
                }
                if (entry.rect.x <= x0 + 0.5 && entry.rect.y <= y0 + 0.5 &&
                    entry.rect.x + entry.rect.width >= x1 - 0.5 &&
                    entry.rect.y + entry.rect.height >= y1 - 0.5)
                {
                    return true;
                }
            }
            return false;
        }

        /// @brief 更新所有覆盖该区域的层的 LRU 时间戳。
        void touch_covering(double x0, double y0, double x1, double y1, uint32_t frame,
                            uint64_t tick) noexcept
        {
            for (DetailCacheEntry &entry : entries_)
            {
                if (!entry.valid || entry.layer < 0 || entry.frame != frame)
                {
                    continue;
                }
                if (entry.rect.x <= x0 + 0.5 && entry.rect.y <= y0 + 0.5 &&
                    entry.rect.x + entry.rect.width >= x1 - 0.5 &&
                    entry.rect.y + entry.rect.height >= y1 - 0.5)
                {
                    entry.last_used = tick;
                }
            }
        }

        /// @brief 把一块区域写入指定层（覆盖该层旧内容）。
        void store(int32_t layer, const voilut::Rect &rect, uint32_t frame,
                   uint64_t tick) noexcept
        {
            if (layer < 0 || static_cast<std::size_t>(layer) >= entries_.size())
            {
                return;
            }
            DetailCacheEntry &entry = entries_[static_cast<std::size_t>(layer)];
            entry.rect = rect;
            entry.layer = layer;
            entry.frame = frame;
            entry.last_used = tick;
            entry.valid = true;
        }

        /// @brief 清空全部缓存。
        void clear() noexcept { entries_ = {}; }

        /// @brief 收集指定帧的有效层（供渲染器写 uniform）。
        /// @param frame 当前帧。
        /// @param out 输出数组（容量至少 kCapacity）。
        /// @return 有效层数量。
        [[nodiscard]] std::size_t collect(uint32_t frame,
                                          std::span<DetailCacheEntry> out) const noexcept
        {
            std::size_t count = 0;
            for (const DetailCacheEntry &entry : entries_)
            {
                if (!entry.valid || entry.layer < 0 || entry.frame != frame ||
                    count >= out.size())
                {
                    continue;
                }
                out[count++] = entry;
            }
            return count;
        }

        /// @brief 只读访问全部层（测试 / 诊断用）。
        [[nodiscard]] const std::array<DetailCacheEntry, kCapacity> &entries() const noexcept
        {
            return entries_;
        }

    private:
        std::array<DetailCacheEntry, kCapacity> entries_{};
    };

    /// @brief 计算下一次细节读取请求（缓存已覆盖可见区域时返回 nullopt）。
    /// @param cache 当前缓存。
    /// @param image_width 原图宽。
    /// @param image_height 原图高。
    /// @param x0 可见区域左上 x（原图坐标）。
    /// @param y0 可见区域左上 y。
    /// @param x1 可见区域右下 x。
    /// @param y1 可见区域右下 y。
    /// @param frame 当前帧。
    /// @param layer_size 细节纹理层边长（默认 2048）。
    /// @return 需要读取的区域；不需要读取（已缓存 / 空区域）时返回 `std::nullopt`。
    [[nodiscard]] inline std::optional<voilut::Rect> plan_detail_request(
        const DetailCache &cache, uint32_t image_width, uint32_t image_height, double x0,
        double y0, double x1, double y1, uint32_t frame, uint32_t layer_size = 2048)
    {
        if (image_width == 0 || image_height == 0)
        {
            return std::nullopt;
        }
        x0 = std::clamp(x0, 0.0, static_cast<double>(image_width));
        x1 = std::clamp(x1, 0.0, static_cast<double>(image_width));
        y0 = std::clamp(y0, 0.0, static_cast<double>(image_height));
        y1 = std::clamp(y1, 0.0, static_cast<double>(image_height));
        if (x1 - x0 < 1.0 || y1 - y0 < 1.0)
        {
            return std::nullopt;
        }
        if (cache.covers(x0, y0, x1, y1, frame))
        {
            return std::nullopt;
        }

        constexpr double kPrefetchMargin = 1.25;
        const uint32_t max_width = std::min(layer_size, image_width);
        const uint32_t max_height = std::min(layer_size, image_height);
        const uint32_t min_width = std::min<uint32_t>(256, max_width);
        const uint32_t min_height = std::min<uint32_t>(256, max_height);
        const uint32_t wanted_width = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::ceil((x1 - x0) * kPrefetchMargin)), min_width,
            max_width);
        const uint32_t wanted_height = std::clamp<uint32_t>(
            static_cast<uint32_t>(std::ceil((y1 - y0) * kPrefetchMargin)), min_height,
            max_height);
        if (wanted_width == 0 || wanted_height == 0)
        {
            return std::nullopt;
        }

        const double center_x = (x0 + x1) * 0.5;
        const double center_y = (y0 + y1) * 0.5;
        auto align = [](double value) {
            return static_cast<uint32_t>(std::max(0.0, std::floor(value / 64.0) * 64.0));
        };
        uint32_t rx = align(center_x - wanted_width * 0.5);
        uint32_t ry = align(center_y - wanted_height * 0.5);
        rx = std::min(rx, image_width - wanted_width);
        ry = std::min(ry, image_height - wanted_height);
        return voilut::Rect{
            .x = static_cast<double>(rx),
            .y = static_cast<double>(ry),
            .width = static_cast<double>(wanted_width),
            .height = static_cast<double>(wanted_height),
        };
    }
} // namespace voilut::app
