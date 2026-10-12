// gpio.hpp - compile-time GPIO driver for STM32F446 (C++20, header-only)
//
// A pin is a *type*: Pin<Port, Number, Config>. Everything about it (which
// register bits to touch, what values to write) is computed by the compiler,
// so using a pin costs no RAM and the generated code is just register accesses.
#pragma once

#include <cstdint>

#include "stm32f4xx.h"

namespace peripheral {
namespace gpio {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

// GPIOA..GPIOH base addresses. The enum value *is* the address.
enum class Port : std::uintptr_t {
  A = GPIOA_BASE,
  B = GPIOB_BASE,
  C = GPIOC_BASE,
  D = GPIOD_BASE,
  E = GPIOE_BASE,
  F = GPIOF_BASE,
  G = GPIOG_BASE,
  H = GPIOH_BASE,
};

// Enum values are the hardware register encodings (MODER, OTYPER, ...).
enum class Mode : u8 { Input = 0, Output = 1, Alternate = 2, Analog = 3 };
enum class OutputType : u8 { PushPull = 0, OpenDrain = 1 };
enum class Speed : u8 { Low = 0, Medium = 1, Fast = 2, High = 3 };
enum class Pull : u8 { None = 0, Up = 1, Down = 2 };
enum class Level : u8 { Low = 0, High = 1 };

struct Config {
  Mode mode = Mode::Input;
  OutputType type = OutputType::PushPull;
  Speed speed = Speed::Low;
  Pull pull = Pull::None;
  u8 af = 0; // alternate function number 0..15, only used by Mode::Alternate
  Level initial = Level::Low; // level driven before the pin becomes an output
};

// Readable config factories, used as the third template argument of Pin.
constexpr Config input(Pull pull = Pull::None) {
  return {.mode = Mode::Input, .pull = pull};
}

constexpr Config output(Speed speed = Speed::Low,
                        OutputType type = OutputType::PushPull,
                        Level initial = Level::Low, Pull pull = Pull::None) {
  return {.mode = Mode::Output,
          .type = type,
          .speed = speed,
          .pull = pull,
          .initial = initial};
}

constexpr Config alternate(u8 af, Speed speed = Speed::Fast,
                           OutputType type = OutputType::PushPull,
                           Pull pull = Pull::None) {
  return {.mode = Mode::Alternate,
          .type = type,
          .speed = speed,
          .pull = pull,
          .af = af};
}

constexpr Config analog() { return {.mode = Mode::Analog}; }

namespace detail {

template <Port P> inline GPIO_TypeDef *regs() {
  return reinterpret_cast<GPIO_TypeDef *>(static_cast<std::uintptr_t>(P));
}

// GPIO ports are 0x400 apart and map to RCC->AHB1ENR bits 0..7 in order.
template <Port P> constexpr u32 clock_bit() {
  return 1u << ((static_cast<std::uintptr_t>(P) - GPIOA_BASE) >> 10);
}

// Configure every pin in `Pins` that lives on port P. All of their register
// fragments are OR-ed together at compile time, so each register of the port
// is read-modify-written exactly once no matter how many pins there are.
template <Port P, typename... Pins>
[[gnu::always_inline]] inline void init_port() {
  if constexpr ((false || ... || (Pins::port == P))) {
    constexpr u32 clr2 = (0u | ... | (Pins::port == P ? Pins::clr2 : 0u));
    constexpr u32 moder = (0u | ... | (Pins::port == P ? Pins::moder : 0u));
    constexpr u32 ospeedr = (0u | ... | (Pins::port == P ? Pins::ospeedr : 0u));
    constexpr u32 pupdr = (0u | ... | (Pins::port == P ? Pins::pupdr : 0u));
    constexpr u32 otyper_clr = (0u | ... | (Pins::port == P ? Pins::mask : 0u));
    constexpr u32 otyper = (0u | ... | (Pins::port == P ? Pins::otyper : 0u));
    constexpr u32 afr_lo_clr =
        (0u | ... | (Pins::port == P ? Pins::afr_lo_clr : 0u));
    constexpr u32 afr_lo = (0u | ... | (Pins::port == P ? Pins::afr_lo : 0u));
    constexpr u32 afr_hi_clr =
        (0u | ... | (Pins::port == P ? Pins::afr_hi_clr : 0u));
    constexpr u32 afr_hi = (0u | ... | (Pins::port == P ? Pins::afr_hi : 0u));
    constexpr u32 bsrr = (0u | ... | (Pins::port == P ? Pins::bsrr : 0u));

    GPIO_TypeDef *const r = regs<P>();

    RCC->AHB1ENR = RCC->AHB1ENR | clock_bit<P>();
    const volatile u32 clock_settle = RCC->AHB1ENR; // let the clock start
    static_cast<void>(clock_settle);

    // Output level first, so the pin never glitches when MODER switches.
    if constexpr (bsrr != 0u) {
      r->BSRR = bsrr;
    }
    if constexpr (afr_lo_clr != 0u) {
      r->AFR[0] = (r->AFR[0] & ~afr_lo_clr) | afr_lo;
    }
    if constexpr (afr_hi_clr != 0u) {
      r->AFR[1] = (r->AFR[1] & ~afr_hi_clr) | afr_hi;
    }
    r->OTYPER = (r->OTYPER & ~otyper_clr) | otyper;
    r->OSPEEDR = (r->OSPEEDR & ~clr2) | ospeedr;
    r->PUPDR = (r->PUPDR & ~clr2) | pupdr;
    r->MODER = (r->MODER & ~clr2) | moder; // last: pin goes live here
  }
}

} // namespace detail

// Initialise any number of pins at once (clocks + registers).
template <typename... Pins> inline void init() {
  detail::init_port<Port::A, Pins...>();
  detail::init_port<Port::B, Pins...>();
  detail::init_port<Port::C, Pins...>();
  detail::init_port<Port::D, Pins...>();
  detail::init_port<Port::E, Pins...>();
  detail::init_port<Port::F, Pins...>();
  detail::init_port<Port::G, Pins...>();
  detail::init_port<Port::H, Pins...>();
}

template <Port P, u8 N, Config C = Config{}> struct Pin {
  static_assert(N < 16, "GPIO pin number must be 0..15");
  static_assert(C.af < 16, "alternate function must be 0..15");

  static constexpr Port port = P;
  static constexpr u8 number = N;
  static constexpr Config config = C;

  // --- compile-time register fragments (consumed by detail::init_port) ---
  static constexpr u32 mask = 1u << N;
  static constexpr u32 clr2 = 3u << (N * 2u);
  static constexpr u32 moder = static_cast<u32>(C.mode) << (N * 2u);
  static constexpr u32 ospeedr = static_cast<u32>(C.speed) << (N * 2u);
  static constexpr u32 pupdr = static_cast<u32>(C.pull) << (N * 2u);
  static constexpr u32 otyper = static_cast<u32>(C.type) << N;
  static constexpr bool is_af = C.mode == Mode::Alternate;
  static constexpr u32 afr_shift = (N & 7u) * 4u;
  static constexpr u32 afr_lo_clr = (is_af && N < 8) ? (0xFu << afr_shift) : 0u;
  static constexpr u32 afr_lo =
      (is_af && N < 8) ? (u32{C.af} << afr_shift) : 0u;
  static constexpr u32 afr_hi_clr =
      (is_af && N >= 8) ? (0xFu << afr_shift) : 0u;
  static constexpr u32 afr_hi =
      (is_af && N >= 8) ? (u32{C.af} << afr_shift) : 0u;
  static constexpr u32 bsrr = C.mode != Mode::Output     ? 0u
                              : C.initial == Level::High ? mask
                                                         : (mask << 16u);

  static void init() { gpio::init<Pin>(); }

  // --- output operations (only compile for output pins) ---
  // BSRR writes are atomic: low half sets, high half resets.
  static void set() {
    static_assert(C.mode == Mode::Output, "set() needs an output pin");
    detail::regs<P>()->BSRR = mask;
  }

  static void reset() {
    static_assert(C.mode == Mode::Output, "reset() needs an output pin");
    detail::regs<P>()->BSRR = mask << 16u;
  }

  static void write(bool high) {
    static_assert(C.mode == Mode::Output, "write() needs an output pin");
    detail::regs<P>()->BSRR = high ? mask : (mask << 16u);
  }

  // Branch-free and atomic: a bit that is set in ODR gets reset, else set.
  static void toggle() {
    static_assert(C.mode == Mode::Output, "toggle() needs an output pin");
    GPIO_TypeDef *const r = detail::regs<P>();
    const u32 odr = r->ODR;
    r->BSRR = ((odr & mask) << 16u) | (~odr & mask);
  }

  // --- input operation ---
  static bool read() {
    static_assert(C.mode != Mode::Analog, "an analog pin has no digital level");
    return (detail::regs<P>()->IDR & mask) != 0u;
  }
};

// Whole-port access, e.g. to drive a parallel bus in a single write.
// The pins must already be configured (see gpio::init). Bit n of every value
// or mask is pin n of the port.
template <Port P> struct PortIO {
  // Read all 16 inputs / only the masked ones.
  static u32 read() { return detail::regs<P>()->IDR; }
  static u32 read(u32 mask) { return detail::regs<P>()->IDR & mask; }

  // Write all 16 outputs at once through ODR.
  static void write(u32 value) { detail::regs<P>()->ODR = value & 0xFFFFu; }

  // Write only the masked pins; other pins are untouched. One atomic BSRR
  // store: 1-bits in `value` set their pin, 0-bits (inside mask) reset it.
  static void write(u32 value, u32 mask) {
    detail::regs<P>()->BSRR = (value & mask) | ((~value & mask) << 16u);
  }

  static void set(u32 pins) { detail::regs<P>()->BSRR = pins; }
  static void reset(u32 pins) { detail::regs<P>()->BSRR = pins << 16u; }

  static void toggle(u32 pins) {
    GPIO_TypeDef *const r = detail::regs<P>();
    const u32 odr = r->ODR;
    r->BSRR = ((odr & pins) << 16u) | (~odr & pins);
  }
};

// A group of `Width` adjacent pins on one port starting at pin `First`,
// accessed as a single number: Bus<Port::B, 0, 8> is PB0..PB7 as a byte.
template <Port P, u8 First, u8 Width> struct Bus {
  static_assert(Width >= 1 && First + Width <= 16, "bus must fit in 0..15");

  static constexpr u32 mask = ((1u << Width) - 1u) << First;

  static void write(u32 value) { PortIO<P>::write(value << First, mask); }
  static u32 read() { return PortIO<P>::read(mask) >> First; }
};

} // namespace gpio
} // namespace peripheral
