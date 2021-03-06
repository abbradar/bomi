#ifndef STATIC_OP_HPP
#define STATIC_OP_HPP

#define CHECK_2(n) \
    static_assert(n && !(n & (n - 1)), "given n is not a power of 2!")

namespace tmp {

namespace detail {

template<int n>
struct is_power_of_2 {
    CHECK_2(n);
};

template<int n>
struct log2 : is_power_of_2<n> {
    SCA value = log2<n/2>::value + 1;
};

template<>
struct log2<1> {
    SCA value = 0;
};

template<int n>
struct aligned : is_power_of_2<n> {
    SCA n_1 = n - 1;
    SCIA get(int value) -> int { return (value + n_1) & ~n_1; }
};

}

template <int n>
SCIA log2() -> int { return detail::log2<n>::value; }

template <int divisor>
SCIA remainder(int n) -> int { CHECK_2(divisor); return n & (divisor - 1); }

template <int n, int n1 = n-1>
SCIA aligned(int value) -> int { return detail::aligned<n>::get(value); }

}

#endif // STATIC_OP_HPP
