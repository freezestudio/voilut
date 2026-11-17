/// @file detail_integration_test.cxx
/// @brief 应用层细节缓存集成测试：缓存命中时不得重复调用 RegionSource::read_region。

#include "detail_cache.h"
#include "region_source.h"

#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <vector>

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        std::printf("  [%s] %s\n", ok ? "OK" : "FAIL", what);
        if (!ok)
        {
            ++g_failures;
        }
    }

    /// @brief 计数版 RegionSource：只关心 read_region 被调用多少次。
    class CountingSource final : public voilut::RegionSource
    {
    public:
        CountingSource()
        {
            info_.width = 20000;
            info_.height = 2000;
            info_.frame_count = 2;
            info_.sample_type = voilut::SampleType::UInt16;
            info_.bits_per_sample = 16;
            info_.name = "counting";
        }

        [[nodiscard]] const voilut::RegionInfo &info() const noexcept override { return info_; }

        [[nodiscard]] std::expected<voilut::DecodedImage, std::string>
        preview(uint32_t /*max_dimension*/) override
        {
            return std::unexpected(std::string("not used"));
        }

        [[nodiscard]] std::expected<voilut::PixelRegion, std::string>
        read_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                    uint32_t frame) override
        {
            ++reads;
            last_frame = frame;
            voilut::PixelRegion region;
            region.x = x;
            region.y = y;
            region.width = width;
            region.height = height;
            region.u16.assign(static_cast<size_t>(width) * height, 0);
            return region;
        }

        int reads = 0;
        uint32_t last_frame = 0;

    private:
        voilut::RegionInfo info_;
    };

    /// @brief 模拟应用层：可见区域 → 规划 → 读取 → 写缓存。
    struct Harness
    {
        voilut::app::DetailCache cache;
        CountingSource source;
        uint64_t tick = 0;

        uint32_t read_for_visible(double x0, double y0, double x1, double y1, uint32_t frame)
        {
            const auto planned = voilut::app::plan_detail_request(
                cache, source.info().width, source.info().height, x0, y0, x1, y1, frame, 2048);
            if (!planned)
            {
                return 0;
            }
            const voilut::Rect &rect = *planned;
            auto region = source.read_region(static_cast<uint32_t>(rect.x),
                                             static_cast<uint32_t>(rect.y),
                                             static_cast<uint32_t>(rect.width),
                                             static_cast<uint32_t>(rect.height), frame);
            if (!region)
            {
                return 0;
            }
            const int32_t layer = cache.pick_layer();
            cache.store(layer, rect, frame, ++tick);
            return 1;
        }
    };
} // namespace

int main()
{
    Harness harness;
    const double x0 = 1000.0;
    const double y0 = 500.0;
    const double x1 = 2280.0; // 1280 像素宽的视口
    const double y1 = 1220.0;

    std::printf("1) 首次请求读取一次\n");
    check(harness.read_for_visible(x0, y0, x1, y1, 0) == 1, "首次规划需要读取");
    check(harness.source.reads == 1, "RegionSource 被调用 1 次");

    std::printf("2) 同一可见区域重复规划不再读取\n");
    for (int i = 0; i < 5; ++i)
    {
        check(harness.read_for_visible(x0, y0, x1, y1, 0) == 0, "缓存命中不产生读取");
    }
    check(harness.source.reads == 1, "RegionSource 仍只被调用 1 次");

    std::printf("3) 预取边距内的小幅平移不再读取\n");
    check(harness.read_for_visible(x0 + 100.0, y0 + 50.0, x1 + 100.0, y1 + 50.0, 0) == 0,
          "25% 预取范围内命中");
    check(harness.source.reads == 1, "RegionSource 仍只被调用 1 次");

    std::printf("4) 移出缓存区域后读取新区域\n");
    check(harness.read_for_visible(12000.0, 1000.0, 13280.0, 1720.0, 0) == 1,
          "远离缓存区域产生新读取");
    check(harness.source.reads == 2, "RegionSource 被调用 2 次");
    check(harness.cache.pick_layer() >= 0, "缓存中仍有可用层");

    std::printf("5) 回到旧区域：命中旧层不再读取\n");
    check(harness.read_for_visible(x0, y0, x1, y1, 0) == 0, "旧区域仍被缓存覆盖");
    check(harness.source.reads == 2, "RegionSource 仍只被调用 2 次");

    std::printf("6) 帧切换后必须重新读取\n");
    check(harness.read_for_visible(x0, y0, x1, y1, 1) == 1, "不同帧不命中");
    check(harness.source.reads == 3, "RegionSource 被调用 3 次");
    check(harness.source.last_frame == 1, "读取的是新帧");

    std::printf("7) 缓存容量满后仍只读取一次\n");
    // 填满 4 层（同一帧的不同区域）。
    for (int i = 0; i < 3; ++i)
    {
        harness.read_for_visible(3000.0 + i * 3000.0, 0.0, 4280.0 + i * 3000.0, 720.0, 1);
    }
    const int before = harness.source.reads;
    check(harness.read_for_visible(3000.0, 0.0, 4280.0, 720.0, 1) == 0, "已缓存区域不读取");
    check(harness.source.reads == before, "LRU 命中不产生额外读取");

    std::printf("\n%s（失败 %d 项）\n", g_failures == 0 ? "全部通过" : "存在失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
