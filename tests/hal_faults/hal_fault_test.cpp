#include "esp_stub.h"
#include "cardenzaHAL.h"

#include <iostream>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void Keyboard(const std::string& fault)
{
  Fake::fail = fault;
  Require(!CardenzaHAL::Init(), "HAL accepted a failed keyboard initialization");
  Require(!CardenzaHAL::LastInitReport().keyboard, "keyboard success was falsely reported");
  Require(Fake::tasks == 0 && Fake::mutexes == 0 && Fake::timers == 0, "failed keyboard initialization leaked resources");
  Fake::fail.clear();
  Require(CardenzaHAL::Init() && CardenzaHAL::LastInitReport().keyboard, "keyboard could not retry");
  Require(Fake::tasks == 1 && Fake::mutexes == 1 && Fake::timers == 1, "keyboard retry acquired duplicate resources");
  const int created = Fake::taskCreates;
  Require(CardenzaHAL::Keyboard::init() && CardenzaHAL::Init() && Fake::taskCreates == created,
          "repeated initialization started another scan task");
}

void Display(const std::string& fault)
{
  CardenzaHAL::Display::SetBrightness(140);
  Fake::fail = fault;
  Require(!CardenzaHAL::Display::Init(), "display accepted injected initialization failure");
  const int previousInits = Fake::panelInits;
  Fake::fail.clear();
  Require(CardenzaHAL::Display::Init(), "display could not retry after partial initialization");
  Require(Fake::panelCreates == 1 && Fake::panelInits > previousInits, "display skipped configuration or leaked a panel on retry");
  Require(!Fake::dmaPending && Fake::draws == 0 && Fake::backlightDuty == 0,
          "display initialization sent pixels or revealed an incomplete first frame");
  Require(CardenzaHAL::Display::GetBrightness() == 140, "display retry lost requested brightness");
  const int draws = Fake::draws;
  Require(CardenzaHAL::Display::Init() && Fake::draws == draws, "healthy display initialization cleared the screen again");
}

void Codec(bool wrongState)
{
  using namespace CardenzaHAL::Audio;
  Require(!SetVolume(42) && Fake::i2cCalls == 0 && GetTxChannel() == nullptr,
          "volume setter initialized audio implicitly");
  if (wrongState) Fake::registers[0x0c] = 0;
  else Fake::fail = "i2c";
  Require(!Init() && !IsCodecReady(), "HAL audio initialization accepted a bad codec");
  Require(GetTxChannel() != nullptr && !GetTxChannel()->enabled, "failed codec startup left temporary TX clocks running");
  const int calls = Fake::i2cCalls;
  Require(!Init() && Fake::i2cCalls == calls, "codec retry ignored its backoff");
  Fake::fail.clear(); Fake::registers[0x0c] = 0x60; Fake::now += 1000001;
  Require(Init() && IsCodecReady() && !GetTxChannel()->enabled, "HAL could not initialize codec on retry and leave TX ready");
  Require(Fake::registers[0x14] == (42 * 191 + 50) / 100, "codec recovery lost requested volume");
  Require(i2s_channel_enable(GetTxChannel()) == ESP_OK, "TX enable failed");
  Fake::fail = "i2c";
  Require(!SetVolume(61) && !IsCodecReady(), "failed volume write was reported successful");
  Fake::fail.clear(); Fake::now += 1000001;
  const int failedCalls = Fake::i2cCalls;
  Require(!SetVolume(61) && Fake::i2cCalls == failedCalls, "volume setter took over codec recovery");
  Require(Init() && GetTxChannel()->enabled && Fake::registers[0x14] == (61 * 191 + 50) / 100,
          "HAL recovery stopped caller-owned TX or lost volume");
  Require(StandbyCodec() && !IsCodecReady() && Fake::registers[0x14] == 0, "standby did not mute output");
  const int standbyCalls = Fake::i2cCalls;
  Require(!SetVolume(28) && !Init() && Fake::i2cCalls == standbyCalls,
          "volume update or ordinary Init woke a codec while the microphone owned its pin");
  Require(i2s_channel_disable(GetTxChannel()) == ESP_OK && RestoreOutputPins(), "TX route restore failed");
  Require(Init() && !GetTxChannel()->enabled && Fake::registers[0x14] == (28 * 191 + 50) / 100,
          "HAL did not restore the cached level after standby");
}

void Storage(bool unmountFailure)
{
  using namespace CardenzaHAL::Storage;
  Require(Init() && IsMounted() && GetCapacity() == 1024 * 512, "initial SD mount failed");
  Require(Init() && Fake::mounts == 1 && Fake::statusChecks == 0, "healthy mount added card probing");
  Require(CheckAfterIoError() && IsMounted(), "a file-level error incorrectly unmounted a responding card");
  Fake::cardResponds = false;
  Require(!CheckAfterIoError() && !IsMounted() && GetCapacity() == 0, "lost card remained mounted");
  Require(Fake::unmounts == 0, "failure notification immediately tore down the filesystem");
  Fake::cardResponds = true;
  if (unmountFailure)
  {
    Fake::fail = "unmount";
    Require(!Init() && !IsMounted(), "VFS teardown failure was ignored");
    Fake::fail.clear();
  }
  Require(Init() && IsMounted() && Fake::mounts == 2 && Fake::unmounts == 1,
          "card replacement did not release the old card exactly once and mount again");
}

void Infrared()
{
  Fake::fail = "carrier";
  Require(!CardenzaHAL::IR::Init(), "carrier setup error was ignored");
  Fake::fail.clear();
  Require(CardenzaHAL::IR::Init() && Fake::carrierHz == 38000 && Fake::carrierDuty > 0.3f && Fake::carrierDuty < 0.4f,
          "NEC carrier was not configured");
  for (unsigned address = 0; address < 256; ++address)
    for (unsigned command = 0; command < 256; ++command)
    {
      CardenzaHAL::IR::Send(address, command);
      const auto word = Fake::irFrame;
      Require((word & 255) == address && ((word >> 8) & 255) == (address ^ 255) &&
              ((word >> 16) & 255) == command && (word >> 24) == (command ^ 255),
              "NEC payload/inverse bytes were corrupted");
    }
}

void InitOwnership()
{
  Fake::fail = "i2c";
  Require(!CardenzaHAL::Init() && !CardenzaHAL::LastInitReport().audio,
          "HAL reported ready before codec initialization succeeded");
  Fake::fail.clear(); Fake::now += 1000001;
  Require(CardenzaHAL::Init() && CardenzaHAL::LastInitReport().audio &&
          CardenzaHAL::Audio::IsCodecReady() && !CardenzaHAL::Audio::GetTxChannel()->enabled,
          "HAL initialization did not own codec setup and restore stopped TX");
}

void InitStopFailure()
{
  Fake::fail = "speaker_disable";
  Require(!CardenzaHAL::Audio::Init() && !CardenzaHAL::Audio::IsCodecReady(),
          "temporary TX shutdown failure was hidden");
  Fake::fail.clear(); Fake::now += 1000001;
  Require(CardenzaHAL::Audio::Init() && !CardenzaHAL::Audio::GetTxChannel()->enabled,
          "HAL could not retire its temporary startup clocks on retry");
}

}

int main(int argc, char** argv)
{
  try
  {
    Require(argc == 3, "expected module and fault");
    const std::string module = argv[1], fault = argv[2];
    if (module == "keyboard") Keyboard(fault);
    else if (module == "display") Display(fault);
    else if (module == "codec") { if (fault == "ownership") InitOwnership(); else if (fault == "stop") InitStopFailure(); else Codec(fault == "state"); }
    else if (module == "storage") Storage(fault == "unmount");
    else if (module == "ir") Infrared();
    else throw std::runtime_error("unknown module");
    std::cout << module << '/' << fault << " passed\n";
  }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
