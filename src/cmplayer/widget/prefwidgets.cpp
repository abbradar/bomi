#include "prefwidgets.hpp"
#include "video/hwacc.hpp"

HwAccCodecBox::HwAccCodecBox(QWidget *parent)
    : QGroupBox(parent)
{
    auto vbox = new QVBoxLayout;
    for (const auto codec : HwAcc::fullCodecList()) {
        auto box = m_checks[codec] = new QCheckBox;
        const auto supported = HwAcc::supports(codec);
        const QString desc = HwAcc::codecDescription(codec);
        if (supported)
            box->setText(desc);
        else
            box->setText(desc % " ("_a % tr("Not supported") % ')'_q);
        box->setEnabled(supported);
        vbox->addWidget(box);
    }
    setLayout(vbox);
}

auto HwAccCodecBox::value() const -> QStringList
{
    QStringList list;
    for (auto it = m_checks.begin(); it != m_checks.end(); ++it)
        if (it.value()->isChecked())
            list.push_back(it.key());
    return list;
}

auto HwAccCodecBox::setValue(const QStringList &list) -> void
{
    for (auto it = m_checks.begin(); it != m_checks.end(); ++it)
        it.value()->setChecked(list.contains(it.key()));
}

/******************************************************************************/

DataButtonGroup::DataButtonGroup(QObject *parent)
    : QButtonGroup(parent)
{
    connect(this, static_cast<Signal<QButtonGroup, int>>(&QButtonGroup::buttonClicked), this, [=] () {
        if (_Change(m_button, checkedButton()))
            emit currentDataChanged(currentData());
    });
}

auto DataButtonGroup::addButton(QAbstractButton *button, const QVariant &data) -> void
{
    QButtonGroup::addButton(button);
    m_data[button] = data;
}

auto DataButtonGroup::button(const QVariant &data) const -> QAbstractButton*
{
    for (auto it = m_data.begin(); it != m_data.end(); ++it) {
        if (*it == data)
            return it.key();
    }
    return nullptr;
}

auto DataButtonGroup::setCurrentData(const QVariant &data) -> void
{
    auto button = this->button(data);
    if (button)
        button->setChecked(true);
    if (_Change(m_button, button))
        emit currentDataChanged(currentData());
}
