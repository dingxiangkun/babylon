#pragma once

#include "babylon/environment.h"
#include "babylon/concurrent/thread_local.h"     // CompactEnumerableThreadLocal

#ifdef __x86_64__
#include <x86intrin.h>
#endif // __x86_64__

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif // __ARM_NEON

BABYLON_NAMESPACE_BEGIN

// 高并发累加计数器
// 原理上等价于利用std::atomic
// 计数操作进行fetch_add(value)
// 读取操作进行load(value)
//
// 实现上针对多写少读的场景做了优化，更适用于典型的计数场景
// 计数操作改为独立发生在对应的线程局部数据上，避免了缓存行竞争
// 但相应的读操作就需要遍历所有线程局部数据并再次累加才能得到结果
class ConcurrentAdder {
public:
    ConcurrentAdder() noexcept = default;
    ConcurrentAdder(ConcurrentAdder&&) noexcept = default;
    ConcurrentAdder(const ConcurrentAdder&) = delete;
    ConcurrentAdder& operator=(ConcurrentAdder&&) noexcept = default;
    ConcurrentAdder& operator=(const ConcurrentAdder&) = delete;

    // 分散计数接口
    inline ConcurrentAdder& operator<<(ssize_t value) noexcept;

    // 汇聚读取接口
    ssize_t value() const noexcept;

    // 重置接口
    void reset() noexcept;

private:
    CompactEnumerableThreadLocal<ssize_t, 64> _storage;
};

// 高并发最大值计数器
// 原理上等价于利用std::atomic
// 计数操作进行loop cas(old, max(new, old))
// 读取&重置操作进行exchange(0)
//
// 实现上针对多写少读的场景做了优化，更适用于典型的计数场景
// 计数操作改为独立发生在对应的线程局部数据上，避免了缓存行竞争
// 但相应的读操作就需要遍历所有线程局部数据并再次累加才能得到结果
class ConcurrentMaxer {
public:
    ConcurrentMaxer() noexcept = default;
    ConcurrentMaxer(ConcurrentMaxer&&) = delete;
    ConcurrentMaxer(const ConcurrentMaxer&) = delete;
    ConcurrentMaxer& operator=(ConcurrentMaxer&&) = delete;
    ConcurrentMaxer& operator=(const ConcurrentMaxer&) = delete;

    // 分散计数接口
    inline ConcurrentMaxer& operator<<(ssize_t value) noexcept;

    // 汇聚读取接口
    // 如果周期内有计数发生，返回计数最大值
    // 如果周期内没有计数发生，返回0
    ssize_t value() const noexcept;

    // 汇聚读取接口
    // 如果周期内有计数发生，返回true，并将max_value填充为计数最大值
    // 如果周期内没有计数发生，返回false，此时不修改max_value的值
    bool value(ssize_t& max_value) const noexcept;

    // 重置接口，开启一个新的周期
    void reset() noexcept;

private:
    struct Slot {
        size_t version {SIZE_MAX};
        ssize_t value;
    };

    CompactEnumerableThreadLocal<Slot, 64> _storage;
    size_t _version {0};
};

// 高并发求和计数器
// 原理上等价于利用锁同步
// 计数操作进行lock {sum += value; num += 1}
// 读取操作进行lock {sum; num}
//
// 实现上针对多写少读的场景做了优化，更适用于典型的计数场景
// 计数操作改为独立发生在对应的线程局部数据上，避免了缓存行竞争
class ConcurrentSummer {
public:
    // 计数结果二元组，总和&总量
    struct Summary {
        ssize_t sum;
        size_t num;
    };

    ConcurrentSummer() noexcept = default;
    ConcurrentSummer(ConcurrentSummer&&) = delete;
    ConcurrentSummer(const ConcurrentSummer&) = delete;
    ConcurrentSummer& operator=(ConcurrentSummer&&) = delete;
    ConcurrentSummer& operator=(const ConcurrentSummer&) = delete;

    // 分散计数接口
    // sum += value; num += 1;
    inline ConcurrentSummer& operator<<(ssize_t value) noexcept;

    // 分散计数接口
    // sum += summary.sum; num += summary.num;
    inline ConcurrentSummer& operator<<(Summary summary) noexcept;

    // 汇聚读取接口
    Summary value() const noexcept;

private:
    CompactEnumerableThreadLocal<Summary, 64> _storage;
};

inline ABSL_ATTRIBUTE_ALWAYS_INLINE
ConcurrentAdder& ConcurrentAdder::operator<<(ssize_t value) noexcept {
    auto& local = _storage.local();
    // local的唯一修改者是自己，所以这里不需要使用原子加法
    // 正确对齐的数值类型赋值本身是原子发生的
    local = local + value;
    return *this;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE
ConcurrentMaxer& ConcurrentMaxer::operator<<(ssize_t value) noexcept {
    auto& local = _storage.local();
    // 为了避免原子操作引入内存屏障，这里采用异步版本感知机制
    // 取代了cas动作来实现重置效果
    //
    // 理论上存在一个缺陷，即
    // 1、刚刚完成一次汇聚读取，正在进行版本推进
    // 2、此时进行中的计数动作依然计入了上一个版本
    // 3、下一个汇聚读取周期中，这些样本无法计入
    // 不过对于统计场景，这个影响非常微弱，可忽略不计
    if (ABSL_PREDICT_FALSE(_version != local.version)) {
        local.version = _version;
        local.value = value;
        return *this;
    }
    if (ABSL_PREDICT_FALSE(value > local.value)) {
        local.value = value;
    }
    return *this;
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE
ConcurrentSummer& ConcurrentSummer::operator<<(ssize_t value) noexcept {
    return operator<<({value, 1});
}

inline ABSL_ATTRIBUTE_ALWAYS_INLINE
ConcurrentSummer& ConcurrentSummer::operator<<(Summary summary) noexcept {
    auto& local = _storage.local();
    // 等效语句local += summary
    // 但涉及128bit原子写操作，需要特殊处理
#ifdef __x86_64__
    // 对于128bit写操作，Intel和AMD官方声明中均承诺未原子性
    // 但是参考https://rigtorp.se/isatomic/
    // 当前主流X86服务端CPU，单cacheline内128bit对齐的load/store其实是原子的
    // 这里主动使用sse指令即可保证原子性写入
    __m128i delta_value = _mm_load_si128(reinterpret_cast<__m128i*>(&summary));
    __m128i local_value = _mm_load_si128(reinterpret_cast<__m128i*>(&local));
    local_value = _mm_add_epi64(local_value, delta_value);
    _mm_store_si128(reinterpret_cast<__m128i*>(&local), local_value);
#else
    // Armv8.4-A开始，主流服务端CPU均支持对齐场景下128bit的读写原子性
    // 这里主动使用neon生成128bit写操作
    int64x2_t delta_value = vld1q_s64(reinterpret_cast<const int64_t*>(&summary));
    int64x2_t local_value = vld1q_s64(reinterpret_cast<const int64_t*>(&local));
    local_value = vaddq_s64(local_value, delta_value);
    vst1q_s64(reinterpret_cast<int64_t*>(&local), local_value);
#endif
    return *this;
}

BABYLON_NAMESPACE_END
