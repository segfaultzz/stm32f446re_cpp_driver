# STM32F446RE C++ driver

Bare-metal, register-level C++20 drivers for the STM32F446RE (Cortex-M4F, Nucleo-F446RE).
No HAL, no exceptions, no RTTI, no dynamic allocation. Only CMSIS register definitions are used.

Currently implemented: **GPIO**.

## Project layout

| Path | Purpose |
|---|---|
| `inc/gpio.hpp` | The GPIO driver (header-only). |
| `src/main.cc` | Example: blink the LED on PA5 and read the button on PC13. |
| `src/syscalls.c` | Newlib stubs (`_write`, `_sbrk`, ...) needed by `--specs=nosys.specs`. |
| `CMakeLists.txt` | Fetches CMSIS, sets Cortex-M4 flags and links the firmware. |
| `arm-none-eabi-toolchain.cmake` | Cross-compiler setup. |
| `STM32F446RETx_FLASH.ld` | Linker script. |

## Build

```powershell
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=arm-none-eabi-toolchain.cmake
cmake --build build
```

Output is `build/app.elf`. Release (`-Os`) is the default build type; use
`-DCMAKE_BUILD_TYPE=Debug` for `-Og`. The build does not flash; program the board yourself, e.g.
`STM32_Programmer_CLI -c port=SWD -w build/app.elf -v -rst`.

Convert to hex: `arm-none-eabi-objcopy -O ihex build/app.elf build/app.hex`

## GPIO driver

### Usage

```cpp
#include "gpio.hpp"

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

### How it works

**A pin is a type.** `Pin<Port, Number, Config>` takes the port, pin number and full
configuration as template parameters. Nothing is stored at runtime, so a pin has no object, no RAM
and no lookup table. `Config` is a plain struct used as a C++20 *class-type non-type template
parameter*, which is why the project builds with `-std=c++20`.

**Registers are computed at compile time.** Each pin exposes `constexpr` register fragments, for
example `moder = mode << (N*2)` and `afr_lo = af << ((N & 7)*4)`. Nothing is computed at run time.

**One read-modify-write per register.** `gpio::init<Pins...>()` calls `detail::init_port` for each
of the 8 ports. For a given port, it ORs together (with a C++17 fold expression) the fragments of
every pin on that port, then writes each register once. Ports with no pins compile to nothing
(`if constexpr`). Initialising 5 pins on GPIOA costs the same register accesses as 1 pin.

**Safe init order.**
1. Enable the port clock in `RCC->AHB1ENR`, with a read-back so the clock is stable before the
   first access. The clock bit is `(port address - GPIOA_BASE) >> 10`, because ports are 0x400
   apart, so no `switch` is needed.
2. Write the initial output level to `BSRR`.
3. Write alternate function, output type, speed and pull.
4. Write `MODER` last, which is when the pin goes live, so it never glitches.

**Atomic output.** `set`, `reset` and `write` store to `BSRR` (low 16 bits set a pin, high 16 bits
reset it), so there is no read-modify-write on `ODR` for an interrupt to corrupt. `toggle` is
branch-free: `BSRR = ((odr & m) << 16) | (~odr & m)`.

**Misuse is a compile error.** `set()`/`toggle()` on an input pin, or `read()` on an analog pin,
trigger a `static_assert`, and so does a pin number above 15 or an AF above 15.

**Verified cost.** In a Release build, `main` compiles to straight-line register loads and stores
with all masks as immediates: `Led::toggle()` is a load of `ODR`, a few ALU ops and one store to
`BSRR`. The whole example is about 1 KB of flash.

### Limits and next steps

- Pin configuration is fixed at compile time. Switching a pin's mode at run time (for example a
  bidirectional data line) would need a small `set_mode()` added.
- External interrupts (EXTI/SYSCFG) and pin locking (`LCKR`) are not implemented yet.
- `src/syscalls.c` is the only C file. Newlib's libc expects those stubs, and the CMSIS startup
  and `system_stm32f4xx.c` are also C. All driver code is C++.
