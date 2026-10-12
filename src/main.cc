#include <cstdint>

#include "gpio.hpp"
#include "uart.hpp"

namespace gpio = peripheral::gpio;
namespace uart = peripheral::uart;

// Nucleo-F446RE: user LED on PA5, user button on PC13 (active low).
using Button = gpio::Pin<gpio::Port::C, 13, gpio::input(gpio::Pull::Up)>;
using Led = gpio::Pin<gpio::Port::A, 5,
                      gpio::output(gpio::Speed::Low, gpio::OutputType::PushPull,
                                   gpio::Level::Low)>;

// Nucleo-F446RE ST-LINK virtual COM port: USART2 on PA2 (TX) / PA3 (RX), AF7.
using UartTx = gpio::Pin<gpio::Port::A, 2, gpio::alternate(7)>;
using UartRx =
    gpio::Pin<gpio::Port::A, 3,
              gpio::alternate(7, gpio::Speed::Fast, gpio::OutputType::PushPull,
                              gpio::Pull::Up)>;
using Console =
    uart::UartIrq<uart::Instance::Usart2, uart::Config{.baud = 115200}>;

extern "C" void USART2_IRQHandler() { Console::on_interrupt(); }

using PortA = gpio::Bus<gpio::Port::A, 0, 7>;

static void delay(std::uint32_t n) {
  for (std::uint32_t i = 0; i < n; ++i) {
    __NOP(); // keeps the loop from being optimized away
  }
}

int main() {
  gpio::init<UartTx, UartRx>();
  Console::init();

  while (true) {
    std::uint8_t c;
    while (Console::try_read(c)) {
      Console::write(c); // echo what was received
    }
    Console::puts("hello\r\n");
    delay(500000);
  }
}
