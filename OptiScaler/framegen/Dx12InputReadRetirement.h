#pragma once
#include <cstddef>
// All pending reads must be submitted before waiting. A recording slot's fence
// value has been reserved but cannot complete until that list is submitted.
template<class Submit, class Wait>
bool RetireDx12InputReaders(std::size_t slots, Submit&& submit, Wait&& wait)
{
    for (std::size_t slot = 0; slot < slots; ++slot)
        if (!submit(slot)) return false;
    for (std::size_t slot = 0; slot < slots; ++slot)
        if (!wait(slot)) return false;
    return true;
}
