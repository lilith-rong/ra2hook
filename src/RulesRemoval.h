#pragma once

class CCINIClass;

namespace RulesRemoval {
    // Startup only: parse the whole optional remove/rules layer, then erase
    // explicit keys. Returns false on a rejected layer or failed native erase.
    bool Apply(CCINIClass* rules);
}
