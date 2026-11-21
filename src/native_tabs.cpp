#include "native_tabs.h"
#include "pr/kit_input.h"
#include "pr/ui.h"

namespace nexus {
void drawTextEditor(PrKit* kit, PrLayoutNode parent, Model& model, bool canFocus) {
    const auto& tab = model.tab(model.active());
    const auto id = "editor-" + std::to_string(tab.id);
    PrKitTextArea area{};
    area.key = prUiKey(kit->ui, parent, {id.data(), id.size()});
    area.text = {tab.draft.data(), tab.draft.size()};
    area.rows = 12;
    const auto node = prKitTextArea(kit, parent, area, {});
    PrKitPlace place{};
    place.flex = PrKitFlex_Fill;
    prKitPlace(kit, node, &place);
    if (canFocus && prUiFocus(kit->ui) != node) prUiSetFocus(kit->ui, node);
    PrStringView changed{};
    if (prKitTextChanged(kit, node, &changed))
        model.edit_extension(model.active(), std::string(changed.data, changed.size));
}
NativeTabs nativeTabTypes() {
    return {{"text_editor", drawTextEditor}};
}
}
