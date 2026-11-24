// usb/drive_logic.h: when the SD card goes to the computer as a USB drive and when it comes back to the firmware,
// through every transition (eject, unplug at each step, a USB reset, suspend and resume, a stop during a transfer,
// sharing twice, a card that does not come back) and the bounds of a read or write.
#include "usb/drive_logic.h"

#include <cassert>
#include <iostream>

using namespace CardenzaHAL::USB::DriveLogic;

namespace {

// Lends and presents: the machine ends Shared with the computer seen.
Machine SharedMachine()
{
    Machine machine;
    assert(machine.Share(true, true, true) == Action::Lend);
    assert(machine.GetState() == State::Starting && machine.CardLent() && !machine.MediumReady());
    assert(machine.Lent(true) == Action::Present);
    assert(machine.Presented(true) == Action::None);
    assert(machine.GetState() == State::Shared && machine.MediumReady());
    machine.HostCommand();
    assert(machine.HostSeen());
    return machine;
}

void Back(Machine& machine, Reason why, bool mounted = true)
{
    assert(machine.GetState() == State::Returning && machine.GetReason() == why && !machine.MediumReady());
    assert(machine.CardLent());
    assert(machine.Returned(mounted) == Action::None);
    assert(machine.GetState() == (mounted ? State::Off : State::Failed));
    assert(!machine.CardLent());
}

void ShareAndStop()
{
    Machine machine = SharedMachine();
    assert(machine.Share(true, true, true) == Action::None);  // sharing twice: nothing more
    assert(machine.Stop() == Action::Return);
    assert(machine.Stop() == Action::None);  // stopping twice: nothing more
    Back(machine, Reason::Stopped);
    assert(machine.Stop() == Action::None);  // stopping when off: nothing
    // And again: a second share after the first.
    assert(machine.Share(true, true, true) == Action::Lend);
    assert(machine.Lent(true) == Action::Present && machine.Presented(true) == Action::None);
    assert(machine.GetState() == State::Shared && machine.GetReason() == Reason::None && !machine.HostSeen());
}

void Refusals()
{
    Machine machine;
    assert(machine.Share(false, true, true) == Action::None && machine.GetState() == State::Failed &&
           machine.GetReason() == Reason::NotBuilt);
    assert(machine.Share(true, false, true) == Action::None && machine.GetReason() == Reason::NotStarted);
    assert(machine.Share(true, true, false) == Action::None && machine.GetReason() == Reason::NoHost);
    assert(!machine.CardLent());
    // After a refusal, a share that can happen does.
    assert(machine.Share(true, true, true) == Action::Lend);
}

void LendFailures()
{
    {
        Machine machine;  // no card: the file system takes the card (none) back, Failed NoCard
        machine.Share(true, true, true);
        assert(machine.Lent(false, Reason::NoCard) == Action::Return);
        assert(machine.Returned(false) == Action::None);
        assert(machine.GetState() == State::Failed && machine.GetReason() == Reason::NoCard);
    }
    {
        Machine machine;  // the file system would not let go
        machine.Share(true, true, true);
        assert(machine.Lent(false, Reason::CardError) == Action::Return);
        assert(machine.Returned(true) == Action::None);
        assert(machine.GetState() == State::Failed && machine.GetReason() == Reason::CardError);
    }
    {
        Machine machine;  // no endpoint for the drive
        machine.Share(true, true, true);
        assert(machine.Lent(true) == Action::Present);
        assert(machine.Presented(false) == Action::Return);
        assert(machine.Returned(true) == Action::None);
        assert(machine.GetState() == State::Failed && machine.GetReason() == Reason::NoRoom);
    }
}

void Eject()
{
    Machine machine = SharedMachine();
    assert(machine.HostEject() == Action::Return);
    assert(!machine.MediumReady());
    assert(machine.HostEject() == Action::None);  // a second eject while returning
    Back(machine, Reason::Ejected);
    // Re-share after an eject.
    assert(machine.Share(true, true, true) == Action::Lend);
    machine.Lent(true);
    machine.Presented(true);
    assert(machine.MediumReady());
}

void Unplug()
{
    {
        Machine machine = SharedMachine();  // the cable goes after the computer used the disk
        assert(machine.HostGone() == Action::Return);
        Back(machine, Reason::Unplugged);
    }
    {
        // While starting (the device's own re-enumeration when the drive is added): not counted.
        Machine machine;
        machine.Share(true, true, true);
        assert(machine.HostGone() == Action::None);
        machine.Lent(true);
        assert(machine.HostGone() == Action::None);
        machine.Presented(true);
        assert(machine.HostGone() == Action::None);  // shared but not used yet: the old configuration going
        assert(machine.GetState() == State::Shared);
    }
    {
        // Unplugged while giving back, and while off: nothing more to do.
        Machine machine = SharedMachine();
        machine.Stop();
        assert(machine.HostGone() == Action::None);
        Back(machine, Reason::Stopped);
        assert(machine.HostGone() == Action::None);
    }
}

void HostAway()
{
    {
        Machine machine = SharedMachine();  // asleep, or a cable pulled on battery: suspended
        uint32_t waited = 0;
        Action action = Action::None;
        while (action == Action::None && waited < 60000)
        {
            action = machine.Tick(250, false);
            waited += 250;
        }
        assert(action == Action::Return && waited == kHostAwayMs);
        Back(machine, Reason::HostAway);
    }
    {
        // Suspended a while, then back (a short sleep, Linux's autosuspend): the share goes on.
        Machine machine = SharedMachine();
        for (uint32_t waited = 0; waited + 250 < kHostAwayMs; waited += 250) assert(machine.Tick(250, false) == Action::None);
        assert(machine.Tick(250, true) == Action::None);  // resumed
        for (uint32_t waited = 0; waited + 250 < kHostAwayMs; waited += 250) assert(machine.Tick(250, false) == Action::None);
        assert(machine.GetState() == State::Shared);
    }
    {
        // Nobody ever uses the drive (nothing plugged in): the card comes back after kHostFirstMs.
        Machine machine;
        machine.Share(true, true, true);
        machine.Lent(true);
        machine.Presented(true);
        uint32_t waited = 0;
        Action action = Action::None;
        while (action == Action::None && waited < 60000)
        {
            action = machine.Tick(250, true);
            waited += 250;
        }
        assert(action == Action::Return && waited == kHostFirstMs);
        Back(machine, Reason::NoHost);
    }
    {
        // A USB reset during the share: the host configures the device again, commands go on, nothing changes.
        Machine machine = SharedMachine();
        assert(machine.Tick(250, false) == Action::None);
        assert(machine.Tick(250, true) == Action::None);
        machine.HostCommand();
        assert(machine.GetState() == State::Shared && machine.MediumReady());
    }
}

void StopWhileStarting()
{
    Machine machine;
    machine.Share(true, true, true);
    assert(machine.Stop() == Action::None);  // remembered
    assert(machine.Lent(true) == Action::Present);
    assert(machine.Presented(true) == Action::Return);  // then given back at once
    Back(machine, Reason::Stopped);
}

void ShareWhileReturning()
{
    Machine machine = SharedMachine();
    machine.HostEject();
    assert(machine.Share(true, true, true) == Action::None);  // remembered
    assert(machine.Returned(true) == Action::Lend);            // then lent again
    assert(machine.GetState() == State::Starting);
    // ... and a stop in between forgets it.
    Machine other = SharedMachine();
    other.HostEject();
    other.Share(true, true, true);
    other.Stop();
    assert(other.Returned(true) == Action::None && other.GetState() == State::Off);
}

void CardGone()
{
    // The card taken out while shared: the file system cannot mount it when it comes back.
    Machine machine = SharedMachine();
    machine.HostGone();
    assert(machine.Returned(false) == Action::None);
    assert(machine.GetState() == State::Failed && machine.GetReason() == Reason::CardError);
    // A later share starts afresh.
    assert(machine.Share(true, true, true) == Action::Lend);
}

void Blocks()
{
    // 4 KB at a time, within the card.
    assert(BlockRangeOk(0, 4096, 0, 1000));
    assert(BlockRangeOk(992, 4096, 0, 1000));      // the last 8 sectors
    assert(!BlockRangeOk(993, 4096, 0, 1000));     // one past the end
    assert(BlockRangeOk(0xFFFFFFF0u, 4096, 0, 0xFFFFFFFFu));   // the end of the largest card
    assert(!BlockRangeOk(0xFFFFFFFCu, 4096, 0, 0xFFFFFFFFu));  // past it: no wrap-around
    assert(!BlockRangeOk(0, 4000, 0, 1000));       // not whole sectors
    assert(!BlockRangeOk(0, 512, 256, 1000));      // inside a sector
    assert(!BlockRangeOk(0, 0, 0, 1000));
}

}  // namespace

int main()
{
    ShareAndStop();
    Refusals();
    LendFailures();
    Eject();
    Unplug();
    HostAway();
    StopWhileStarting();
    ShareWhileReturning();
    CardGone();
    Blocks();
    std::cout << "drive tests passed\n";
    return 0;
}
