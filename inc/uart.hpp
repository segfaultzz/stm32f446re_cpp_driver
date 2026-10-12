// uart.hpp - compile-time UART/USART driver for STM32F446 (C++20, header-only)
//
// A port is a *type*: Uart<Instance, Config>. The baud divisor and the CR1/CR2/
// CR3 values are computed by the compiler, so init() is just a few register
// writes. One class covers both transfer styles, chosen in Config:
//
//   Transfer::Polling    busy-waits on the status flags (default)
//   Transfer::Interrupt  ring buffers filled/drained by the USART ISR
//   (Transfer::Dma       planned)
//
// The functions are the same either way:
//
//   using Console = peripheral::uart::Uart<
//       peripheral::uart::Instance::Usart2,
//       peripheral::uart::Config{.baud = 115200,
//                                .transfer = peripheral::uart::Transfer::Interrupt}>;
//   extern "C" void USART2_IRQHandler() { Console::on_interrupt(); } // Interrupt only
//
//   gpio::init<Tx, Rx>();   // pins must be set to their alternate function
//   Console::init();
//   Console::puts("hello\r\n");
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "stm32f4xx.h"

namespace peripheral {
namespace uart {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

// The enum value *is* the peripheral base address.
enum class Instance : std::uintptr_t {
  Usart1 = USART1_BASE, // APB2
  Usart2 = USART2_BASE, // APB1
  Usart3 = USART3_BASE, // APB1
  Uart4 = UART4_BASE,   // APB1
  Uart5 = UART5_BASE,   // APB1
  Usart6 = USART6_BASE  // APB2
};

// The values below are the register bits they stand for, so Config can be
// turned into CR1/CR2/CR3 with plain OR.
enum class StopBits : u32 { One = 0u, Two = USART_CR2_STOP_1 };

// Parity uses one of the data bits: Eight + parity = 7 data bits + parity.
// For 8 data bits plus parity choose Nine.
enum class WordLength : u32 { Eight = 0u, Nine = USART_CR1_M };

enum class Parity : u32 {
  None = 0u,
  Even = USART_CR1_PCE,
  Odd = USART_CR1_PCE | USART_CR1_PS
};

enum class Mode : u32 {
  Tx = USART_CR1_TE,
  Rx = USART_CR1_RE,
  TxRx = USART_CR1_TE | USART_CR1_RE
};

// RTS/CTS exist on USART1/2/3/6 only.
enum class FlowControl : u32 {
  None = 0u,
  Rts = USART_CR3_RTSE,
  Cts = USART_CR3_CTSE,
  RtsCts = USART_CR3_RTSE | USART_CR3_CTSE
};

enum class Oversampling : u32 { By16 = 0u, By8 = USART_CR1_OVER8 };

// How bytes move between your code and the data register.
enum class Transfer : u8 { Polling, Interrupt };

struct Config {
  u32 baud = 115200;
  // Clock of the bus the peripheral sits on. After reset the chip runs from
  // HSI (16 MHz) with APB prescalers of 1; change it if you set up the PLL.
  u32 pclk = 16'000'000;
  Mode mode = Mode::TxRx;
  Transfer transfer = Transfer::Polling;
  // Ring buffer sizes (powers of two), only used by Transfer::Interrupt.
  u32 rx_buffer_size = 128;
  u32 tx_buffer_size = 128;
  Oversampling oversampling = Oversampling::By16;
  FlowControl flow = FlowControl::None;
  Parity parity = Parity::None;
  WordLength length = WordLength::Eight;
  StopBits stop = StopBits::One;
};

namespace detail {

template <Instance I> inline USART_TypeDef *regs() {
  return reinterpret_cast<USART_TypeDef *>(static_cast<std::uintptr_t>(I));
}

template <Instance I> constexpr bool on_apb2() {
  return I == Instance::Usart1 || I == Instance::Usart6;
}

template <Instance I> constexpr bool has_flow_control() {
  return I != Instance::Uart4 && I != Instance::Uart5;
}

template <Instance I> constexpr u32 clock_bit() {
  switch (I) {
  case Instance::Usart1: return RCC_APB2ENR_USART1EN;
  case Instance::Usart6: return RCC_APB2ENR_USART6EN;
  case Instance::Usart2: return RCC_APB1ENR_USART2EN;
  case Instance::Usart3: return RCC_APB1ENR_USART3EN;
  case Instance::Uart4: return RCC_APB1ENR_UART4EN;
  case Instance::Uart5: return RCC_APB1ENR_UART5EN;
  }
  return 0u;
}

template <Instance I> constexpr IRQn_Type irq_number() {
  switch (I) {
  case Instance::Usart1: return USART1_IRQn;
  case Instance::Usart2: return USART2_IRQn;
  case Instance::Usart3: return USART3_IRQn;
  case Instance::Uart4: return UART4_IRQn;
  case Instance::Uart5: return UART5_IRQn;
  case Instance::Usart6: return USART6_IRQn;
  }
  return USART1_IRQn;
}

constexpr bool has(Mode m, Mode bit) {
  return (static_cast<u32>(m) & static_cast<u32>(bit)) != 0u;
}

constexpr bool is_pow2(u32 n) { return n >= 2u && (n & (n - 1u)) == 0u; }

// Lock-free single-producer / single-consumer queue. Only the producer writes
// `head`, only the consumer writes `tail`, so no interrupt masking is needed
// between main code and the ISR. The indices count freely and wrap at 2^32;
// because N is a power of two, `index & (N - 1)` is still the right slot.
template <u32 N> struct RingBuffer {
  static_assert(is_pow2(N), "buffer size must be a power of two");

  u8 data[N]{};
  std::atomic<u32> head{0};
  std::atomic<u32> tail{0};

  u32 size() const {
    return head.load(std::memory_order_acquire) -
           tail.load(std::memory_order_acquire);
  }
  bool empty() const { return size() == 0u; }

  bool push(u8 byte) { // producer side
    const u32 h = head.load(std::memory_order_relaxed);
    if (h - tail.load(std::memory_order_acquire) == N) {
      return false; // full
    }
    data[h & (N - 1u)] = byte;
    head.store(h + 1u, std::memory_order_release); // publish after the data
    return true;
  }

  bool pop(u8 &byte) { // consumer side
    const u32 t = tail.load(std::memory_order_relaxed);
    if (head.load(std::memory_order_acquire) == t) {
      return false; // empty
    }
    byte = data[t & (N - 1u)];
    tail.store(t + 1u, std::memory_order_release); // free the slot last
    return true;
  }

  void clear() { // only while nothing is using the buffer
    head.store(0u, std::memory_order_relaxed);
    tail.store(0u, std::memory_order_relaxed);
  }
};

} // namespace detail

template <Instance I, Config C = Config{}> struct Uart {
  static_assert(C.baud > 0u, "baud rate must be non-zero");
  static_assert(C.flow == FlowControl::None || detail::has_flow_control<I>(),
                "UART4/UART5 have no RTS/CTS");

  static constexpr bool interrupt = C.transfer == Transfer::Interrupt;
  static constexpr bool tx_enabled = detail::has(C.mode, Mode::Tx);
  static constexpr bool rx_enabled = detail::has(C.mode, Mode::Rx);
  static constexpr IRQn_Type irq = detail::irq_number<I>();

  static_assert(!interrupt || detail::is_pow2(C.rx_buffer_size),
                "rx_buffer_size must be a power of two");
  static_assert(!interrupt || detail::is_pow2(C.tx_buffer_size),
                "tx_buffer_size must be a power of two");

  // --- baud rate ---
  static constexpr bool over8 = C.oversampling == Oversampling::By8;

  // x = pclk / baud rounded to nearest, in units of 1/16 (By16) or 1/8 (By8)
  // of a clock tick: this is USARTDIV scaled by 16 or 8.
  static constexpr u32 x = (C.pclk + C.baud / 2u) / C.baud;
  static_assert(x >= (over8 ? 8u : 16u), "baud rate too high for this pclk");

  // By16: BRR = mantissa<<4 | 4-bit fraction, which is just x.
  // By8:  BRR = mantissa<<4 | 3-bit fraction (bit 3 must stay 0).
  static constexpr u32 brr = over8 ? (((x >> 3) << 4) | (x & 7u)) : x;

  // Real baud rate and its error in 1/10 000, so a bad config fails to build.
  static constexpr u32 actual_baud = C.pclk / x;
  static constexpr u32 error_x10k =
      (actual_baud > C.baud ? actual_baud - C.baud : C.baud - actual_baud) *
      10'000u / C.baud;
  static_assert(error_x10k <= 300u, "baud rate error above 3 %");

  // --- register values ---
  static constexpr u32 cr1 =
      static_cast<u32>(C.mode) | static_cast<u32>(C.oversampling) |
      static_cast<u32>(C.parity) | static_cast<u32>(C.length);
  static constexpr u32 cr2 = static_cast<u32>(C.stop);
  static constexpr u32 cr3 = static_cast<u32>(C.flow);

  // --- interrupt mode state (never instantiated in polling mode) ---
  static inline detail::RingBuffer<C.rx_buffer_size> rx_buf;
  static inline detail::RingBuffer<C.tx_buffer_size> tx_buf;

  // Written only by the ISR, so plain volatile is enough.
  static inline volatile u32 rx_overflow_count = 0u; // RX buffer was full
  static inline volatile u32 rx_error_count = 0u; // overrun/framing/noise/parity

  // Enable the clock and start the peripheral.
  static void init() {
    constexpr u32 bit = detail::clock_bit<I>();
    if constexpr (detail::on_apb2<I>()) {
      RCC->APB2ENR = RCC->APB2ENR | bit;
      const volatile u32 settle = RCC->APB2ENR; // let the clock start
      static_cast<void>(settle);
    } else {
      RCC->APB1ENR = RCC->APB1ENR | bit;
      const volatile u32 settle = RCC->APB1ENR;
      static_cast<void>(settle);
    }

    if constexpr (interrupt) {
      rx_buf.clear();
      tx_buf.clear();
    }

    USART_TypeDef *const r = detail::regs<I>();
    r->CR1 = 0u; // UE off while configuring
    r->CR2 = cr2;
    r->CR3 = cr3;
    r->BRR = brr;
    r->CR1 = cr1 | USART_CR1_UE; // last: peripheral goes live here

    if constexpr (interrupt) {
      if constexpr (rx_enabled) {
        r->CR1 = r->CR1 | USART_CR1_RXNEIE;
      }
      NVIC_EnableIRQ(irq);
    }
  }

  // Call from the USART's IRQ handler (Transfer::Interrupt only).
  static void on_interrupt() {
    static_assert(interrupt, "on_interrupt() needs Transfer::Interrupt");
    USART_TypeDef *const r = detail::regs<I>();
    const u32 sr = r->SR;

    if constexpr (rx_enabled) {
      if ((sr & (USART_SR_RXNE | USART_SR_ORE)) != 0u) {
        // Reading SR (above) and then DR clears RXNE and the error flags.
        const u8 byte = static_cast<u8>(r->DR);
        if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE | USART_SR_PE)) !=
            0u) {
          rx_error_count = rx_error_count + 1u;
        }
        if ((sr & USART_SR_RXNE) != 0u && !rx_buf.push(byte)) {
          rx_overflow_count = rx_overflow_count + 1u;
        }
      }
    }

    if constexpr (tx_enabled) {
      if ((sr & USART_SR_TXE) != 0u && (r->CR1 & USART_CR1_TXEIE) != 0u) {
        u8 byte;
        if (tx_buf.pop(byte)) {
          r->DR = byte;
        } else {
          r->CR1 = r->CR1 & ~USART_CR1_TXEIE; // nothing left: stop TXE irqs
        }
      }
    }
  }

  // --- transmit ---
  // Send one byte without waiting. Polling: false if the data register is
  // still busy. Interrupt: false if the TX buffer is full.
  static bool try_write(u8 byte) {
    static_assert(tx_enabled, "writing needs Mode::Tx or Mode::TxRx");
    USART_TypeDef *const r = detail::regs<I>();
    if constexpr (interrupt) {
      if (!tx_buf.push(byte)) {
        return false;
      }
      r->CR1 = r->CR1 | USART_CR1_TXEIE; // wake the ISR after the data is queued
      return true;
    } else {
      if ((r->SR & USART_SR_TXE) == 0u) {
        return false;
      }
      r->DR = byte;
      return true;
    }
  }

  // Waits until the byte is accepted. In interrupt mode this needs interrupts
  // enabled, otherwise a full buffer is never drained.
  static void write(u8 byte) {
    while (!try_write(byte)) {
    }
  }

  static void write(const void *data, std::size_t len) {
    const u8 *p = static_cast<const u8 *>(data);
    while (len--) {
      write(*p++);
    }
  }

  static void puts(const char *s) {
    while (*s != '\0') {
      write(static_cast<u8>(*s++));
    }
  }

  // Block until everything has fully left the shift register.
  static void flush() {
    static_assert(tx_enabled, "flush() needs Mode::Tx or Mode::TxRx");
    if constexpr (interrupt) {
      while (!tx_buf.empty()) {
      }
    }
    while ((detail::regs<I>()->SR & USART_SR_TC) == 0u) {
    }
  }

  // --- receive ---
  // Bytes ready to read. Polling: 0 or 1 (the data register holds one byte).
  // Interrupt: how many are waiting in the RX buffer.
  static u32 available() {
    static_assert(rx_enabled, "reading needs Mode::Rx or Mode::TxRx");
    if constexpr (interrupt) {
      return rx_buf.size();
    } else {
      return (detail::regs<I>()->SR & USART_SR_RXNE) != 0u ? 1u : 0u;
    }
  }

  // Non-blocking read: returns false when nothing is waiting.
  static bool try_read(u8 &out) {
    static_assert(rx_enabled, "reading needs Mode::Rx or Mode::TxRx");
    if constexpr (interrupt) {
      return rx_buf.pop(out);
    } else {
      USART_TypeDef *const r = detail::regs<I>();
      if ((r->SR & USART_SR_RXNE) == 0u) {
        return false;
      }
      out = static_cast<u8>(r->DR); // SR then DR also clears error flags
      return true;
    }
  }

  // Blocks until a byte arrives.
  static u8 read() {
    u8 byte;
    while (!try_read(byte)) {
    }
    return byte;
  }
};

} // namespace uart
} // namespace peripheral
