#pragma once

#include <compare>
#include <cstdint>

namespace mercury {

// Buy rests on the bid side; Sell rests on the ask side.
enum class Side : std::uint8_t { Buy, Sell };

// Strong typedefs keep IDs / prices / sizes from mixing by accident.
class OrderId {
 public:
  constexpr explicit OrderId(std::uint64_t value) noexcept : value_(value) {}
  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr auto operator<=>(const OrderId&) const = default;

 private:
  std::uint64_t value_;
};

class AccountId {
 public:
  constexpr explicit AccountId(std::uint64_t value) noexcept : value_(value) {}
  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr auto operator<=>(const AccountId&) const = default;

 private:
  std::uint64_t value_;
};

// Price in integer ticks. Matching never uses floating point.
class Price {
 public:
  constexpr explicit Price(std::int64_t ticks) noexcept : ticks_(ticks) {}
  constexpr std::int64_t ticks() const noexcept { return ticks_; }
  constexpr auto operator<=>(const Price&) const = default;

 private:
  std::int64_t ticks_;
};

class Quantity {
 public:
  constexpr explicit Quantity(std::uint64_t value) noexcept : value_(value) {}
  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }
  constexpr auto operator<=>(const Quantity&) const = default;

 private:
  std::uint64_t value_;
};

constexpr Quantity operator-(Quantity lhs, Quantity rhs) {
  return Quantity{lhs.value() - rhs.value()};
}

}  // namespace mercury
