/// @file detail_cache.h
/// @brief 细节区域的 LRU 缓存策略（不依赖 GPU / ImGui，可单测）。
/// @details 只负责“选层、覆盖判断、LRU 时间戳、按帧收集”；纹理数组由渲染器持有。

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
} // namespace voilut::app
