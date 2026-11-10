#include "input.h"

namespace nexus {

PrAppModifierFlags withoutCapsLock(PrAppModifierFlags modifiers) {
    return PrAppModifierFlags(modifiers & ~PrAppModifierFlag_CapsLock);
}

} // namespace nexus
