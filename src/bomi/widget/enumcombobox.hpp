#ifndef ENUMCOMBOBOX_HPP
#define ENUMCOMBOBOX_HPP

#include "datacombobox.hpp"
#include "enum/generateplaylist.hpp"
#include "enum/autoloadmode.hpp"
#include "enum/autoselectmode.hpp"
#include "enum/clippingmethod.hpp"
#include "enum/interpolator.hpp"
#include "enum/textthemestyle.hpp"
#include "enum/channellayout.hpp"
#include "enum/verticalalignment.hpp"

template<class Enum>
class EnumComboBox : public DataComboBox {
public:
    EnumComboBox(QWidget *parent = 0): DataComboBox(parent) {
        setup(this);
        retranslate(this);
    }
    auto currentValue() const -> Enum { return currentData().template value<Enum>(); }
    auto setCurrentValue(Enum e) -> void { setCurrentData(QVariant::fromValue(e)); }
    auto setRetranslatable(bool retrans) -> void { m_retrans = retrans; }
private:
    static auto setup(QComboBox *combo) -> void {
        combo->clear();
        for (auto &item : EnumInfo<Enum>::items())
            combo->addItem(QString(), QVariant::fromValue(item.value));
    }
    static auto retranslate(QComboBox *combo) -> void {
        const auto items = EnumInfo<Enum>::items();
        Q_ASSERT((int)items.size() == combo->count());
        for (int i=0; i<(int)items.size(); ++i)
            combo->setItemText(i, EnumInfo<Enum>::description(items[i].value));
    }
    auto changeEvent(QEvent *event) -> void {
        QComboBox::changeEvent(event);
        if (event->type() == QEvent::LanguageChange && m_retrans)
            retranslate(this);
    }
    bool m_retrans = true;
};

using GeneratePlaylistComboBox = EnumComboBox<GeneratePlaylist>;
using AutoloadModeComboBox = EnumComboBox<AutoloadMode>;
using AutoselectModeComboBox = EnumComboBox<AutoselectMode>;
using ClippingMethodComboBox = EnumComboBox<ClippingMethod>;
using InterpolatorComboBox = EnumComboBox<Interpolator>;
using TextThemeStyleComboBox = EnumComboBox<TextThemeStyle>;
using ChannelComboBox = EnumComboBox<ChannelLayout>;
using VerticalAlignmentComboBox = EnumComboBox<VerticalAlignment>;

#endif // ENUMCOMBOBOX_HPP
