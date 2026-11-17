/// @file detail_cache_test.cxx
/// @brief 细节区域 LRU 缓存策略测试（无 GPU / ImGui 依赖）。

#include "detail_cache.h"

#include <array>
#include <cstdint>
#include <cstdio>

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

    voilut::Rect rect(double x, double y, double width, double height)
    {
        return {.x = x, .y = y, .width = width, .height = height};
    }
} // namespace

int main()
{
    voilut::app::DetailCache cache;

    std::printf("1) 空缓存与层选择\n");
    check(cache.pick_layer() == 0, "空缓存优先第 0 层");
    check(!cache.covers(0.0, 0.0, 10.0, 10.0, 0), "空缓存不覆盖任何区域");

    std::printf("2) 存满 4 层后的 LRU\n");
    for (int32_t layer = 0; layer < 4; ++layer)
    {
        cache.store(layer, rect(layer * 100.0, 0.0, 100.0, 100.0), 0,
                    static_cast<uint64_t>(layer + 1));
    }
    check(cache.pick_layer() == 0, "无空闲层时选最久未用（layer 0）");
    cache.touch_covering(0.0, 0.0, 50.0, 50.0, 0, 11);
    check(cache.pick_layer() == 1, "touch layer 0 后最久未用变成 layer 1");

    std::printf("3) 覆盖判断（帧 / 边界）\n");
    // 覆盖当前区域（含 0.5 容差）。
    check(cache.covers(20.0, 20.0, 80.0, 80.0, 0), "同帧内部区域被覆盖");
    check(cache.covers(0.0, 0.0, 100.0, 100.0, 0), "同帧完整区域被覆盖");
    check(!cache.covers(90.0, 90.0, 110.0, 110.0, 0), "超出边界不覆盖");
    check(!cache.covers(20.0, 20.0, 80.0, 80.0, 9), "不同帧不覆盖");
    check(!cache.covers(-10.0, 0.0, 50.0, 50.0, 0), "左侧越界不覆盖");

    std::printf("4) 按帧收集有效层\n");
    std::array<voilut::app::DetailCacheEntry, voilut::app::DetailCache::kCapacity> collected{};
    check(cache.collect(0, collected) == 4, "frame 0 收集到 4 层");
    check(collected[0].layer == 0 && collected[0].frame == 0, "收集内容正确");
    cache.store(3, rect(300.0, 0.0, 100.0, 100.0), 1, 20);
    check(cache.collect(0, collected) == 3, "frame 0 过滤掉 frame 1");
    check(cache.collect(1, collected) == 1, "frame 1 只收集到 1 层");

    std::printf("5) 清空\n");
    cache.clear();
    check(cache.pick_layer() == 0, "清空后重新从第 0 层开始");
    check(!cache.covers(0.0, 0.0, 100.0, 100.0, 0), "清空后无覆盖");
    check(cache.collect(0, collected) == 0, "清空后收集为空");

    std::printf("\n%s（失败 %d 项）\n", g_failures == 0 ? "全部通过" : "存在失败", g_failures);
    return g_failures == 0 ? 0 : 1;
}
