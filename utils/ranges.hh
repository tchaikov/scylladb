#pragma once

#include <concepts>
#include <ranges>
#include <iterator>
#include <functional>

namespace utils {

template<class R>
concept unique_range =
    std::ranges::input_range<R> &&
    std::ranges::view<R>;

template<std::ranges::view V>
class unique_view : public std::ranges::view_interface<unique_view<V>> {
private:
    V base_ = V();

    template<bool Const>
    class iterator {
    private:
        using Base = std::conditional_t<Const, const V, V>;
        using BaseIter = std::ranges::iterator_t<Base>;
        using BaseSent = std::ranges::sentinel_t<Base>;

        BaseIter current_ = BaseIter();
        BaseSent end_ = BaseSent();
        BaseIter next_ = BaseIter();

    public:
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::ranges::range_value_t<Base>;
        using difference_type = std::ranges::range_difference_t<Base>;

        iterator() = default;

        constexpr iterator(BaseIter current, BaseSent end)
            : current_(current), end_(end) {
            if (current_ != end_) {
                next_ = std::ranges::next(current_);
                skip_duplicates();
            }
        }

        constexpr const auto& operator*() const noexcept {
            return *current_;
        }

        constexpr iterator& operator++() {
            current_ = next_;
            if (current_ != end_) {
                next_ = std::ranges::next(current_);
                skip_duplicates();
            }
            return *this;
        }

        constexpr iterator operator++(int) {
            auto tmp = *this;
            ++*this;
            return tmp;
        }

        friend constexpr bool operator==(const iterator& x, const iterator& y) {
            return x.current_ == y.current_;
        }

    private:
        constexpr void skip_duplicates() {
            while (next_ != end_ && *next_ == *current_) {
                ++next_;
            }
        }
    };

public:
    unique_view() = default;

    constexpr explicit unique_view(V base)
        : base_(std::move(base)) {}

    constexpr V base() const & { return base_; }
    constexpr V base() && { return std::move(base_); }

    constexpr auto begin() {
        return iterator<false>(std::ranges::begin(base_), std::ranges::end(base_));
    }

    constexpr auto begin() const {
        return iterator<true>(std::ranges::begin(base_), std::ranges::end(base_));
    }

    constexpr auto end() {
        return iterator<false>(std::ranges::end(base_), std::ranges::end(base_));
    }

    constexpr auto end() const {
        return iterator<true>(std::ranges::end(base_), std::ranges::end(base_));
    }
};

template<class R>
unique_view(R&&) -> unique_view<std::views::all_t<R>>;

namespace detail {
    struct unique_range_adaptor {
        template<std::ranges::viewable_range R>
        constexpr auto operator()(R&& r) const {
            return unique_view(std::forward<R>(r));
        }
    };
}

namespace views {
    inline constexpr detail::unique_range_adaptor uniqued;
}

} // namespace utils
