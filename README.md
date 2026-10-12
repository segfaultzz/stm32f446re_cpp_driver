# STM32F446RE C++ driver

Bare-metal, register-level C++20 drivers for the STM32F446RE (Cortex-M4F, Nucleo-F446RE).
No HAL, no exceptions, no RTTI, no dynamic allocation. Only CMSIS register definitions are used.

Implemented: **GPIO**, **UART/USART** (polling and interrupt-driven; DMA planned).

This README is also a learning guide: it explains every file and the ideas behind the code.

## Contents

1. [Project layout](#project-layout)
2. [Build](#build)
3. [Ideas used everywhere](#ideas-used-everywhere)
4. [GPIO driver](#gpio-driver)
5. [UART driver](#uart-driver)
6. [main.cc walkthrough](#maincc-walkthrough)
7. [Build system and low-level files](#build-system-and-low-level-files)
8. [Limits and next steps](#limits-and-next-steps)

## Project layout

| Path | Purpose |
|---|---|
| `inc/gpio.hpp` | GPIO driver (header-only), namespace `peripheral::gpio`. |
| `inc/uart.hpp` | UART driver (header-only), namespace `peripheral::uart`. |
| `src/main.cc` | Example: LED/button definitions and a UART echo over the ST-LINK virtual COM port. |
| `src/syscalls.c` | Newlib stubs (`_write`, `_sbrk`, ...) needed by `--specs=nosys.specs`. |
| `src/uart.cc` | Old stub, not part of the build; safe to delete. |
| `CMakeLists.txt` | Fetches CMSIS, sets Cortex-M4 flags and links the firmware. |
| `arm-none-eabi-toolchain.cmake` | Cross-compiler setup. |
| `STM32F446RETx_FLASH.ld` | Linker script (memory map). |

## Build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=arm-none-eabi-toolchain.cmake
cmake --build build
```

Output is `build/app.elf`. Release (`-Os`) is the default build type; use
`-DCMAKE_BUILD_TYPE=Debug` for `-Og`. The build does not flash; program the board yourself, e.g.
`STM32_Programmer_CLI -c port=SWD -w build/app.elf -v -rst`.

Convert to hex: `arm-none-eabi-objcopy -O ihex build/app.elf build/app.hex`

## Ideas used everywhere

These four ideas explain why the drivers look the way they do.

### 1. Peripherals are memory

On ARM Cortex-M, a peripheral's registers are ordinary addresses. Writing a 32-bit value to
`0x40004404` configures the USART2 baud rate. CMSIS gives these addresses names and layouts:

```cpp
USART2->BRR = 139;      // USART2 is a pointer to a struct laid over the peripheral's memory
```

Registers are declared `volatile` in CMSIS, which tells the compiler "every access matters, do not
remove or reorder it".

### 2. Read-modify-write

Most registers hold many unrelated fields. To change one field without touching the others:

```cpp
reg = (reg & ~mask) | value;   // clear the field, then set the new value
```

### 3. Compile-time computation (`constexpr`)

Everything that can be known before the program runs (bit masks, baud divisor, which clock bit to
enable) is computed by the compiler. The generated code is then only "load, OR with a constant,
store". There is no lookup table and no runtime arithmetic.

### 4. A peripheral is a type, not an object

```cpp
using Led = gpio::Pin<gpio::Port::A, 5, gpio::output()>;
Led::set();
```

`Led` is a type with only `static` functions. There is no `Led` variable, so it uses zero RAM, and
the compiler knows everything about it at compile time. The settings are passed as **template
parameters**. C++20 allows a whole `struct` (such as `Config`) to be a template parameter, as long as
all its members are public and of simple types; this is why the project needs `-std=c++20`.

Misuse is caught by `static_assert`, which produces a compile error with a readable message instead
of a bug on the board.

## GPIO driver

### Usage

```cpp
#include "gpio.hpp"
namespace gpio = peripheral::gpio;

using Led    = gpio::Pin<gpio::Port::A, 5, gpio::output()>;
using Button = gpio::Pin<gpio::Port::C, 13, gpio::input(gpio::Pull::Up)>;

int main() {
  gpio::init<Led, Button>();      // enable clocks + configure registers

  Led::set();                     // drive high
  Led::reset();                   // drive low
  Led::toggle();
  Led::write(true);
  bool pressed = !Button::read(); // Nucleo button is active low
}
```

Config factories (all `constexpr`):

| Factory | Meaning |
|---|---|
| `gpio::input(Pull)` | Digital input, optional pull-up/down. |
| `gpio::output(Speed, OutputType, Level initial, Pull)` | Push-pull (default) or open-drain output. |
| `gpio::alternate(af, Speed, OutputType, Pull)` | Alternate function 0..15 (UART, SPI, I2C, timers...). |
| `gpio::analog()` | Analog mode (ADC/DAC), digital buffer off. |

Examples:

```cpp
using Tx  = gpio::Pin<gpio::Port::A, 2, gpio::alternate(7)>;   // USART2_TX is AF7
using Rx  = gpio::Pin<gpio::Port::A, 3, gpio::alternate(7, gpio::Speed::Fast,
                                                         gpio::OutputType::PushPull,
                                                         gpio::Pull::Up)>;
using Sda = gpio::Pin<gpio::Port::B, 9, gpio::alternate(4, gpio::Speed::Fast,
                                                         gpio::OutputType::OpenDrain,
                                                         gpio::Pull::Up)>;  // I2C1_SDA
using Adc = gpio::Pin<gpio::Port::A, 0, gpio::analog()>;

gpio::init<Tx, Rx, Sda, Adc>();
```

Port and bus access (pins must be configured first with `gpio::init`):

```cpp
using Port_B = gpio::PortIO<gpio::Port::B>;

Port_B::write(0x00FF);              // all 16 outputs via ODR
Port_B::write(0x0005, 0x000F);      // only PB0..PB3 = 0b0101, other pins untouched (atomic, BSRR)
Port_B::set(0x0003);                // PB0, PB1 high
Port_B::reset(0x0003);
Port_B::toggle(0x0030);
uint32_t all = Port_B::read();      // all 16 inputs
uint32_t low = Port_B::read(0x000F);// only the masked pins

using Data = gpio::Bus<gpio::Port::B, 0, 8>;  // PB0..PB7 as one byte
Data::write(0xA5);                  // changes only those 8 pins, atomically
uint8_t v = Data::read();           // returns the value shifted down to bit 0
```

### gpio.hpp, section by section

**Enums.** `Port`, `Mode`, `OutputType`, `Speed`, `Pull`, `Level`. `Port` values are the port base
addresses (`GPIOA_BASE`...), the other enums use the exact values the hardware registers expect
(for example `Mode::Alternate = 2` is the bit pattern `0b10` in `MODER`). So an enum value can be
shifted straight into a register.

**`Config` and the factories.** `Config` bundles all settings of one pin. `input()`, `output()`,
`alternate()` and `analog()` build a `Config` with C++20 designated initialisers
(`{.mode = Mode::Output, .speed = speed}`), so the pin definition reads like a sentence.

**`detail::regs<P>()`.** Turns the `Port` value (an address) into a `GPIO_TypeDef*`, so code can
write `regs<P>()->BSRR = ...`. `detail` holds helpers that are not meant for users.

**`detail::clock_bit<P>()`.** GPIO ports are `0x400` bytes apart and their clock-enable bits in
`RCC->AHB1ENR` are in the same order, so bit number = `(address - GPIOA_BASE) >> 10`. No `switch`
needed.

**`Pin<Port, Number, Config>`.** The core type. Its `static constexpr` members are register
fragments, each already shifted into place:

| Member | Meaning |
|---|---|
| `mask` | `1 << N`, the pin's bit in 1-bit-per-pin registers (`OTYPER`, `ODR`, `BSRR`). |
| `clr2` | `3 << (N*2)`, clears the pin's 2-bit field in `MODER`/`OSPEEDR`/`PUPDR`. |
| `moder`, `ospeedr`, `pupdr` | The value of the pin's 2-bit field. |
| `otyper` | The value of the pin's 1-bit field. |
| `afr_lo`, `afr_hi` (+ `_clr`) | Alternate function nibble. Pins 0-7 live in `AFR[0]`, 8-15 in `AFR[1]`, 4 bits each. |
| `bsrr` | The initial output level, written before the pin becomes an output. |

**`init_port<P, Pins...>()`.** For one port, this ORs together (a C++17 *fold expression*) the
fragments of every pin that belongs to the port, then does one read-modify-write per register.
`Pins::port == P ? Pins::moder : 0u` means "only count pins of this port". If no pin is on the port,
`if constexpr` removes the whole function body. Five pins on GPIOA cost the same register accesses
as one.

**`init<Pins...>()`.** Calls `init_port` for all 8 ports. The compiler removes the unused ones.

**Safe init order**
1. Enable the port clock in `RCC->AHB1ENR`, then read it back so the clock is running before the
   first register access.
2. Write the initial output level to `BSRR`.
3. Write alternate function, output type, speed and pull.
4. Write `MODER` last. That is when the pin goes live, so it never glitches.

**Atomic output.** `set`, `reset` and `write` store to `BSRR`: bits 0-15 set a pin, bits 16-31 reset
it, and bits written as 0 do nothing. So there is no read-modify-write on `ODR` that an interrupt
could corrupt. `toggle` is branch-free: `BSRR = ((odr & m) << 16) | (~odr & m)` resets the pins that
are high and sets the ones that are low.

**`PortIO<Port>`.** Whole-port operations (read all 16 pins, write masked bits atomically, ...).

**`Bus<Port, First, Width>`.** A group of adjacent pins used as one number. `Bus<Port::B, 0, 8>` is
PB0..PB7 as a byte; the mask and shift are compile-time constants.

**Misuse is a compile error.** `set()`/`toggle()` on an input pin, `read()` on an analog pin, a
pin number above 15 or an AF above 15 all fail with a `static_assert`.

**Verified cost.** In a Release build, `main` compiles to straight-line register loads and stores
with all masks as immediates.

## UART driver

### Usage

One class, `Uart<Instance, Config>`. The transfer style is a config option, so switching from
polling to interrupts is a one-word change and the rest of your code stays the same.

```cpp
#include "gpio.hpp"
#include "uart.hpp"
namespace gpio = peripheral::gpio;
namespace uart = peripheral::uart;

using Tx = gpio::Pin<gpio::Port::A, 2, gpio::alternate(7)>;
using Rx = gpio::Pin<gpio::Port::A, 3, gpio::alternate(7, gpio::Speed::Fast,
                                                        gpio::OutputType::PushPull,
                                                        gpio::Pull::Up)>;

// Polling (the default)
using Console = uart::Uart<uart::Instance::Usart2, uart::Config{.baud = 115200}>;

// Interrupt driven: only the Config changes
using Console = uart::Uart<uart::Instance::Usart2,
                           uart::Config{.baud = 115200,
                                        .transfer = uart::Transfer::Interrupt}>;
extern "C" void USART2_IRQHandler() { Console::on_interrupt(); }   // interrupt mode only

int main() {
  gpio::init<Tx, Rx>();           // pins to alternate function
  Console::init();                // clock + UART registers (+ NVIC in interrupt mode)

  Console::puts("hello\r\n");
  Console::write('A');
  uint8_t c;
  if (Console::try_read(c)) { /* got a byte */ }
  uint8_t b = Console::read();    // blocks
  Console::flush();               // wait until the last bit has left the pin
}
```

Config fields (use designated initialisers, **in this order**; skipped ones keep their default):

| Field | Default | Meaning |
|---|---|---|
| `baud` | 115200 | Baud rate. |
| `pclk` | 16 000 000 | Clock of the bus the UART is on (APB1 or APB2). Set it to the real value if you use the PLL. |
| `mode` | `Mode::TxRx` | `Tx`, `Rx` or `TxRx`. |
| `transfer` | `Transfer::Polling` | `Polling` or `Interrupt`. |
| `rx_buffer_size`, `tx_buffer_size` | 128 | Ring buffer sizes (powers of two), interrupt mode only. |
| `oversampling` | `By16` | `By16` or `By8` (By8 allows higher baud rates). |
| `flow` | `None` | `None`, `Rts`, `Cts`, `RtsCts` (USART1/2/3/6 only). |
| `parity` | `None` | `None`, `Even`, `Odd`. |
| `length` | `Eight` | `Eight` or `Nine` data bits (the parity bit counts as one of them). |
| `stop` | `One` | `One` or `Two`. |

Functions (the same in both modes):

| Function | Polling | Interrupt |
|---|---|---|
| `try_write(b)` | sends if the data register is free, else `false` | queues in the TX buffer, `false` if full |
| `write(b)`, `write(ptr, n)`, `puts(s)` | wait until accepted | wait for buffer space |
| `flush()` | wait for `TC` | wait for empty buffer, then `TC` |
| `available()` | 0 or 1 | bytes in the RX buffer |
| `try_read(b)` | `false` if no byte | `false` if buffer empty |
| `read()` | blocks | blocks |
| `on_interrupt()` | not allowed (compile error) | call from the IRQ handler |
| `rx_overflow_count`, `rx_error_count` | unused | counters for dropped bytes / line errors |

### uart.hpp, section by section

**`Instance`.** The six UART/USART peripherals; the value is the base address, as with `gpio::Port`.

**Config enums.** `StopBits`, `WordLength`, `Parity`, `Mode`, `FlowControl`, `Oversampling` hold
the actual register bits (`Parity::Odd = USART_CR1_PCE | USART_CR1_PS`). `Config` is turned into the
three control registers with a plain OR:

```cpp
cr1 = mode | oversampling | parity | length;
cr2 = stop;
cr3 = flow;
```

Parity uses one data bit: `Eight` + parity means 7 data bits + parity. For 8 data bits plus parity
choose `Nine`.

**`detail` helpers.** `regs<I>()` (address to `USART_TypeDef*`), `on_apb2<I>()` (USART1/6 are on
APB2, the rest on APB1), `clock_bit<I>()` (the RCC enable bit), `irq_number<I>()` (the NVIC
interrupt number), `has()` (test a flag in `Mode`).

**Baud rate.** The hardware divides the bus clock by `USARTDIV`, which has a whole part and a
fraction. The `BRR` register stores it as `mantissa << 4 | fraction`.

- With 16x oversampling the fraction is 4 bits, so `BRR = round(pclk / baud)`. Example: 16 MHz /
  115200 = 138.9, rounded 139; the real baud rate is 115107 (error 0.08 %).
- With 8x oversampling the fraction has only 3 bits and bit 3 must be 0, so
  `BRR = ((x >> 3) << 4) | (x & 7)`.
- `static_assert`s refuse a baud rate that is too high for the clock or has more than 3 % error.

**`init()`**
1. Enable the peripheral clock (`APB1ENR` or `APB2ENR`, chosen at compile time with
   `if constexpr`) and read it back.
2. Interrupt mode: empty the buffers.
3. `CR1 = 0` so the UART is off while it is configured.
4. Write `CR2`, `CR3` and `BRR`.
5. `CR1 = cr1 | UE`, last. The UART starts only now.
6. Interrupt mode: set `RXNEIE` (interrupt when a byte arrives) and enable the interrupt in the
   NVIC.

The pins are **not** configured here. Put them in alternate function mode with `gpio::init`.

**Status flags used**

| Flag | Meaning |
|---|---|
| `TXE` | Transmit data register empty: you may write the next byte to `DR`. |
| `TC` | Transmission complete: the last bit has left the shift register. |
| `RXNE` | Receive data register not empty: a byte is waiting in `DR`. |
| `ORE`/`FE`/`NE`/`PE` | Overrun / framing / noise / parity error. |

Reading `SR` and then `DR` clears `RXNE` and the error flags.

**Polling mode.** `try_write` checks `TXE` and writes `DR`. `try_read` checks `RXNE` and reads `DR`.
`write()` and `read()` are loops around them. The CPU is stuck waiting, which is simple and fine for
tests but wastes time at low baud rates.

**Interrupt mode.** Two ring buffers decouple your code from the hardware:

```
write()  -> TX buffer -> (TXE interrupt) -> DR -> wire
read()   <- RX buffer <- (RXNE interrupt) <- DR <- wire
```

Your code only touches the buffers. The ISR moves bytes between buffers and `DR`.

- **`try_write`** pushes the byte into the TX buffer and then sets `TXEIE`. Push first, enable
  second, so the ISR always finds data.
- **`on_interrupt()` RX part:** if `RXNE` or `ORE` is set, read `DR` (this clears the flags), count
  errors, push into the RX buffer; if the buffer is full the byte is dropped and
  `rx_overflow_count` increases.
- **`on_interrupt()` TX part:** if `TXE` is set and `TXEIE` is enabled, pop one byte into `DR`. If
  the buffer is empty, clear `TXEIE`, otherwise the interrupt would fire forever.
- **Why the ISR is written in your code:** its name (`USART2_IRQHandler`) is fixed by the startup
  file, and a template cannot define a function with a fixed name. `extern "C"` stops C++ name
  mangling, so it replaces the weak default handler. Without it the chip hangs in the default
  handler.
- **Buffers cost nothing in polling mode.** They are `static inline` members, which are only
  instantiated when used.

**`RingBuffer<N>`.** A lock-free queue for exactly one producer and one consumer, which is what
main code vs. ISR is. The producer writes only `head`, the consumer only `tail`, so no interrupt
masking is needed. The indices count up forever and wrap at 2^32; as N is a power of two,
`index & (N-1)` always gives the right slot, and `head - tail` is always the number of bytes
stored. `std::atomic` with acquire/release ordering guarantees the data is stored before `head` moves;
plain `volatile` would not.

**Misuse is a compile error.** Reading on a `Mode::Tx` port, `on_interrupt()` in polling mode, flow
control on UART4/5, a non-power-of-two buffer size.

**Rules for interrupt mode**
- Do not call `write()` with interrupts disabled or from an ISR when the buffer may be full; nothing
  would ever drain it.
- The NVIC priority is the default (0, highest). Set it with `NVIC_SetPriority` if you add more
  interrupts.

## main.cc walkthrough

```cpp
namespace gpio = peripheral::gpio;
namespace uart = peripheral::uart;
```
Short aliases so the code is not full of `peripheral::`.

```cpp
using Button = gpio::Pin<gpio::Port::C, 13, gpio::input(gpio::Pull::Up)>;
using Led    = gpio::Pin<gpio::Port::A, 5, gpio::output(...)>;
```
The Nucleo user button (PC13, pressed = low) and LED (PA5), as types.

```cpp
using UartTx = gpio::Pin<gpio::Port::A, 2, gpio::alternate(7)>;
using UartRx = gpio::Pin<gpio::Port::A, 3, gpio::alternate(7, ..., gpio::Pull::Up)>;
```
PA2/PA3 are wired to the ST-LINK virtual COM port, so the terminal on your PC sees USART2. AF7 is
the alternate function number that connects those pins to USART1-3 (see the datasheet's alternate
function table).

```cpp
using Console = uart::Uart<uart::Instance::Usart2, uart::Config{.baud = 115200, .transfer = uart::Transfer::Interrupt}>;
extern "C" void USART2_IRQHandler() { Console::on_interrupt(); }
```
The UART type and its interrupt handler.

```cpp
static void delay(std::uint32_t n) { for (...) __NOP(); }
```
A crude busy-wait. `__NOP()` stops the compiler from deleting the empty loop.

```cpp
int main() {
  gpio::init<UartTx, UartRx>();   // pins
  Console::init();                // UART
  while (true) {
    while (Console::try_read(c)) Console::write(c);   // echo anything received
    Console::puts("hello\r\n");
    delay(500000);
  }
}
```
Pins first, then the UART. The loop echoes whatever you type and prints `hello` every iteration.

## Build system and low-level files

**`CMakeLists.txt`**
- `FetchContent` downloads two ARM/ST repositories at configure time: **CMSIS core** (Cortex-M
  headers such as `__NOP`, `NVIC_EnableIRQ`) and **cmsis_device_f4** (register definitions
  `stm32f446xx.h`, the startup assembly, `system_stm32f4xx.c`).
- The `cmsis` interface library carries their include paths and the `STM32F446xx` define, which
  selects the right register set.
- The executable is built from `main.cc`, `syscalls.c`, the startup file and `system_stm32f4xx.c`.
- Compile flags: `-mcpu=cortex-m4 -mthumb` (instruction set), `-mfpu=fpv4-sp-d16 -mfloat-abi=hard`
  (use the hardware FPU), `-ffunction-sections -fdata-sections` plus `-Wl,--gc-sections` (drop unused
  code), `-fno-exceptions -fno-rtti` (no C++ runtime overhead), `-Os`/`-Og` per build type.
- Link flags: `-T...ld` (the linker script), `-Wl,-Map` (a map file showing where everything went),
  `--specs=nano.specs` (small libc) and `--specs=nosys.specs` (stub system calls).

**`arm-none-eabi-toolchain.cmake`** tells CMake it is cross-compiling for a bare-metal ARM target
(`CMAKE_SYSTEM_NAME Generic`), names the compilers, and builds test programs as static libraries
(`CMAKE_TRY_COMPILE_TARGET_TYPE`) because a bare-metal program cannot link without a linker script.

**`STM32F446RETx_FLASH.ld`** is the memory map: 512 KB FLASH at `0x08000000` (code, constants and
the initial values of variables), 128 KB RAM at `0x20000000` (variables, heap, stack). `_estack` is
the top of RAM, where the stack starts. The sections are `.isr_vector` (interrupt table, first in
flash), `.text` (code), `.rodata` (constants), `.data` (initialised variables, copied flash -> RAM
at start) and `.bss` (zero-initialised variables).

**Startup file** (`startup_stm32f446xx.s`, from cmsis_device_f4). After reset the CPU reads the
stack pointer and the address of `Reset_Handler` from the vector table. `Reset_Handler` copies
`.data` to RAM, zeroes `.bss`, calls `SystemInit`, then C++ static constructors, then `main`. It also
defines every interrupt handler as a *weak* alias of a default infinite loop; defining a function
with the same name, like `USART2_IRQHandler`, overrides it.

**`system_stm32f4xx.c`** (`SystemInit`) sets up the FPU and the vector table. It does not change the
clock, so the chip runs from the 16 MHz HSI oscillator, which is why `pclk` defaults to 16 MHz.

**`src/syscalls.c`** gives the C library the system functions it expects (`_write`, `_read`,
`_close`, `_sbrk`, ...). They are mostly empty. `_write` is where you would later send `printf`
output to the UART.

## Limits and next steps

- Pin and UART configuration is fixed at compile time. Changing a pin's mode at run time would need
  a small `set_mode()`.
- External interrupts (EXTI/SYSCFG) and pin locking (`LCKR`) are not implemented.
- UART DMA transfers are planned as `Transfer::Dma`, reusing the same `Config`, baud and API.
- The clock is the default 16 MHz HSI; there is no PLL setup yet.
