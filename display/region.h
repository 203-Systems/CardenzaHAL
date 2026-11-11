#pragma once

// The screen's size and a rectangle of it. Pure (no ESP-IDF): host code and tests can include it.
namespace CardenzaHAL::Display {
    constexpr int WIDTH = 240;
    constexpr int HEIGHT = 135;

    struct Region {
        int x;
        int y;
        int width;
        int height;
    };
}
